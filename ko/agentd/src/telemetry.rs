// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

//! Secret scrubbing, plus optional GlitchTip (Sentry envelope) errors and
//! PostHog events. Both are off unless configured; payloads never carry
//! prompts, outputs, secrets or hostnames.

use std::sync::{Arc, LazyLock, RwLock};

use anyhow::{Context, Result, bail};
use regex::Regex;
use reqwest::Url;
use serde_json::{Value, json};
use tokio::sync::Semaphore;
use zeroize::Zeroizing;

use crate::config::{Config, NetworkConfig, PostHogConfig};
use crate::secrets::SecretStore;

const REDACTED: &str = "[REDACTED]";

static SECRET_VALUES: LazyLock<RwLock<Vec<Zeroizing<String>>>> =
    LazyLock::new(|| RwLock::new(Vec::new()));

static SECRET_PATTERNS: LazyLock<Vec<(Regex, &'static str)>> = LazyLock::new(|| {
    [
        (r"(?i)\bbearer\s+[A-Za-z0-9._~+/=-]{6,}", "Bearer [REDACTED]"),
        (r"\b(sk|pk|rk|phx|phc)[-_][A-Za-z0-9_-]{12,}", REDACTED),
        (r"(?i)\b([a-z][a-z0-9+.-]*://)[^/\s@]+@", "${1}[REDACTED]@"),
        (
            r#"(?i)\b(api[_-]?key|access[_-]?token|token|secret|password|authorization|cf-aig-authorization)(["']?\s*[:=]\s*["']?)[^\s"',}&]+"#,
            "${1}${2}[REDACTED]",
        ),
    ]
    .into_iter()
    .map(|(p, r)| (Regex::new(p).expect("valid regex"), r))
    .collect()
});

static TELEMETRY_PATTERNS: LazyLock<Vec<(Regex, &'static str)>> = LazyLock::new(|| {
    [
        (r"[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}", "[EMAIL]"),
        (r"\b\d{1,3}(\.\d{1,3}){3}(:\d+)?\b", "[IP]"),
        (r"\b[0-9a-fA-F]{32}\b", "[ID]"),
        (r"[A-Za-z0-9+/_=-]{40,}", REDACTED),
        (r"(/Users|/home)/[^/\s]+", "~"),
    ]
    .into_iter()
    .map(|(p, r)| (Regex::new(p).expect("valid regex"), r))
    .collect()
});

/// Remembers a secret value so it is scrubbed from logs and telemetry.
pub fn register_secret(value: &str) {
    if value.len() < 8 {
        return;
    }
    let mut g = SECRET_VALUES.write().unwrap_or_else(|e| e.into_inner());
    if g.len() < 512 && !g.iter().any(|s| s.as_str() == value) {
        g.push(Zeroizing::new(value.to_owned()));
    }
}

/// Removes known secret values and credential-looking substrings.
pub fn scrub(s: &str) -> String {
    let mut out = s.to_owned();
    {
        let g = SECRET_VALUES.read().unwrap_or_else(|e| e.into_inner());
        for v in g.iter() {
            if out.contains(v.as_str()) {
                out = out.replace(v.as_str(), REDACTED);
            }
        }
    }
    for (re, rep) in SECRET_PATTERNS.iter() {
        out = re.replace_all(&out, *rep).into_owned();
    }
    out
}

/// Stricter scrub for data leaving the machine: also drops emails, IPs,
/// account-like ids, long tokens and home paths.
pub fn scrub_for_telemetry(s: &str) -> String {
    let mut out = scrub(s);
    for (re, rep) in TELEMETRY_PATTERNS.iter() {
        out = re.replace_all(&out, *rep).into_owned();
    }
    crate::util::truncate(&out, 2048).to_owned()
}

/// Sentry-compatible DSN: https://<key>@<host>[/<prefix>]/<project_id>.
#[derive(Debug, Clone)]
pub struct Dsn {
    pub envelope_url: Url,
    pub public_key: String,
}

impl Dsn {
    pub fn parse(raw: &str, net: &NetworkConfig) -> Result<Self> {
        let mut url = Url::parse(raw.trim()).context("invalid DSN")?;
        let key = url.username().to_owned();
        if key.is_empty() || !key.bytes().all(|b| b.is_ascii_alphanumeric()) {
            bail!("DSN has no valid public key");
        }
        let _ = url.set_username("");
        let _ = url.set_password(None);
        let mut segs: Vec<String> = url
            .path_segments()
            .map(|s| s.filter(|p| !p.is_empty()).map(str::to_owned).collect())
            .unwrap_or_default();
        let project = segs.pop().context("DSN has no project id")?;
        if !project.bytes().all(|b| b.is_ascii_digit()) {
            bail!("DSN project id must be numeric");
        }
        let prefix = if segs.is_empty() {
            String::new()
        } else {
            format!("/{}", segs.join("/"))
        };
        url.set_path(&format!("{prefix}/api/{project}/envelope/"));
        url.set_query(None);
        url.set_fragment(None);
        let envelope_url = crate::config::validate_upstream_url(url.as_str(), net)?;
        Ok(Self {
            envelope_url,
            public_key: key,
        })
    }
}

struct Inner {
    http: reqwest::Client,
    glitchtip: Option<(Dsn, String)>,
    posthog: Option<(Url, PostHogConfig)>,
    install_id: String,
    inflight: Semaphore,
}

#[derive(Clone, Default)]
pub struct Telemetry {
    inner: Option<Arc<Inner>>,
}

impl Telemetry {
    pub fn disabled() -> Self {
        Self { inner: None }
    }

    pub fn new(
        cfg: &Config,
        secrets: &SecretStore,
        http: reqwest::Client,
        install_id: String,
    ) -> Result<Self> {
        let net = &cfg.network;
        let glitchtip = match &cfg.telemetry.glitchtip {
            None => None,
            Some(g) => {
                let dsn = match (&g.dsn_secret, &g.dsn) {
                    (_, Some(d)) => Some(Zeroizing::new(d.clone())),
                    (Some(name), None) => secrets.get(name)?,
                    (None, None) => None,
                };
                match dsn {
                    Some(d) => {
                        register_secret(&d);
                        Some((Dsn::parse(&d, net)?, g.environment.clone()))
                    }
                    None => {
                        tracing::warn!(
                            "telemetry.glitchtip configured but no DSN is set; disabled"
                        );
                        None
                    }
                }
            }
        };
        let posthog = match &cfg.telemetry.posthog {
            None => None,
            Some(p) => {
                let base = crate::config::validate_upstream_url(&p.host, net)?;
                Some((base.join("/i/v0/e/")?, p.clone()))
            }
        };
        if glitchtip.is_none() && posthog.is_none() {
            return Ok(Self::disabled());
        }
        Ok(Self {
            inner: Some(Arc::new(Inner {
                http,
                glitchtip,
                posthog,
                install_id,
                inflight: Semaphore::new(8),
            })),
        })
    }

    pub fn errors_enabled(&self) -> bool {
        self.inner.as_ref().is_some_and(|i| i.glitchtip.is_some())
    }

    pub fn events_enabled(&self) -> bool {
        self.inner.as_ref().is_some_and(|i| i.posthog.is_some())
    }

    /// Fire-and-forget error report. Dropped when too many are in flight.
    pub fn report_error(&self, kind: &str, message: &str) {
        let Some(inner) = self.inner.clone() else {
            return;
        };
        let Some((dsn, env)) = inner.glitchtip.clone() else {
            return;
        };
        let event = error_event(kind, message, &env);
        tokio::spawn(async move {
            let Ok(_permit) = inner.inflight.try_acquire() else {
                return;
            };
            let res = send_envelope(&inner.http, &dsn, &event).await;
            if let Err(e) = res {
                tracing::debug!(error = %scrub(&format!("{e:#}")), "glitchtip send failed");
            }
        });
    }

    /// Fire-and-forget product event. Callers pass only non-identifying props.
    pub fn capture(&self, event: &str, props: Value) {
        let Some(inner) = self.inner.clone() else {
            return;
        };
        let Some((url, ph)) = inner.posthog.clone() else {
            return;
        };
        let body = posthog_body(&ph.project_key, &inner.install_id, event, props);
        tokio::spawn(async move {
            let Ok(_permit) = inner.inflight.try_acquire() else {
                return;
            };
            let res = inner
                .http
                .post(url)
                .json(&body)
                .send()
                .await
                .and_then(|r| r.error_for_status());
            if let Err(e) = res {
                tracing::debug!(error = %scrub(&e.to_string()), "posthog send failed");
            }
        });
    }
}

pub fn error_event(kind: &str, message: &str, environment: &str) -> Value {
    let id = uuid::Uuid::new_v4().simple().to_string();
    json!({
        "event_id": id,
        "timestamp": crate::util::now_ms() as f64 / 1000.0,
        "platform": "other",
        "level": "error",
        "logger": "ko-agentd",
        "release": format!("ko-agentd@{}", crate::VERSION),
        "environment": environment,
        "message": { "formatted": scrub_for_telemetry(message) },
        "tags": { "kind": scrub_for_telemetry(kind) },
        "contexts": { "os": { "name": std::env::consts::OS } },
    })
}

async fn send_envelope(http: &reqwest::Client, dsn: &Dsn, event: &Value) -> Result<()> {
    let header = json!({ "event_id": event["event_id"], "sent_at": crate::util::rfc3339(crate::util::now_ms()) });
    let payload = serde_json::to_string(event)?;
    let body = format!(
        "{}\n{}\n{}\n",
        header,
        json!({ "type": "event", "length": payload.len() }),
        payload
    );
    let auth = format!(
        "Sentry sentry_version=7, sentry_client=ko-agentd/{}, sentry_key={}",
        crate::VERSION,
        dsn.public_key
    );
    http.post(dsn.envelope_url.clone())
        .header("X-Sentry-Auth", auth)
        .header("Content-Type", "application/x-sentry-envelope")
        .body(body)
        .send()
        .await?
        .error_for_status()?;
    Ok(())
}

pub fn posthog_body(key: &str, distinct_id: &str, event: &str, props: Value) -> Value {
    let mut p = match props {
        Value::Object(m) => m,
        _ => serde_json::Map::new(),
    };
    p.insert("$lib".into(), json!("ko-agentd"));
    p.insert("$lib_version".into(), json!(crate::VERSION));
    p.insert("$process_person_profile".into(), json!(false));
    p.insert("$geoip_disable".into(), json!(true));
    p.insert("os".into(), json!(std::env::consts::OS));
    json!({
        "api_key": key,
        "event": event,
        "distinct_id": distinct_id,
        "properties": p,
        "timestamp": crate::util::rfc3339(crate::util::now_ms()),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn scrubs_known_secrets_and_patterns() {
        register_secret("super-secret-value-xyz");
        let s = scrub("error: super-secret-value-xyz rejected");
        assert!(!s.contains("super-secret-value-xyz"));
        assert!(!scrub("Authorization: Bearer abcdef123456").contains("abcdef123456"));
        assert!(!scrub("key sk-proj-abcdefghijklmnop1234").contains("abcdefghijklmnop"));
        assert!(!scrub("socks5h://user:pass@100.64.0.10:1080").contains("pass"));
        assert!(!scrub(r#"{"api_key": "hunter2hunter2"}"#).contains("hunter2"));
        assert!(!scrub("token=abc123&x=1").contains("abc123"));
        // Short registered values are ignored (too many false positives).
        register_secret("abc");
        assert_eq!(scrub("abc"), "abc");
    }

    #[test]
    fn telemetry_scrub_drops_identifying_data() {
        let s = scrub_for_telemetry(
            "connect to 100.64.0.10:1080 failed for user@example.com in /Users/someone/x \
             account 0123456789abcdef0123456789abcdef",
        );
        assert!(!s.contains("100.64"));
        assert!(!s.contains("example.com"));
        assert!(!s.contains("someone"));
        assert!(!s.contains("0123456789abcdef"));
    }

    #[test]
    fn dsn_parsing() {
        let net = NetworkConfig::default();
        let d = Dsn::parse("https://abc123@glitchtip.example.com/7", &net).unwrap();
        assert_eq!(d.public_key, "abc123");
        assert_eq!(
            d.envelope_url.as_str(),
            "https://glitchtip.example.com/api/7/envelope/"
        );
        let d = Dsn::parse("https://k@example.com/sub/path/42", &net).unwrap();
        assert_eq!(
            d.envelope_url.as_str(),
            "https://example.com/sub/path/api/42/envelope/"
        );
        assert!(Dsn::parse("http://k@example.com/1", &net).is_err());
        assert!(Dsn::parse("https://example.com/1", &net).is_err());
        assert!(Dsn::parse("https://k@example.com/abc", &net).is_err());
    }

    #[test]
    fn event_payloads_are_scrubbed() {
        let e = error_event(
            "provider",
            "upstream said: Bearer sk-abcdefghijklmnopqrstuv",
            "test",
        );
        let msg = e["message"]["formatted"].as_str().unwrap();
        assert!(!msg.contains("sk-abc"));
        let b = posthog_body("phc_x", "install", "run_finished", json!({"status": "ok"}));
        assert_eq!(b["properties"]["$process_person_profile"], json!(false));
        assert_eq!(b["properties"]["status"], json!("ok"));
    }
}
