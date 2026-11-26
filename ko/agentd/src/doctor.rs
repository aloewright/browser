// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

//! `ko-agentd doctor`: read-only environment checks. Never prints secrets.

use std::fmt::Write as _;
use std::os::unix::fs::MetadataExt;
use std::process::Stdio;
use std::time::Duration;

use crate::config::Config;
use crate::paths::Paths;
use crate::secrets::SecretStore;

#[derive(Default)]
pub struct Report {
    pub text: String,
    pub failures: usize,
}

impl Report {
    fn line(&mut self, level: &str, msg: impl AsRef<str>) {
        if level == "FAIL" {
            self.failures += 1;
        }
        let _ = writeln!(self.text, "[{level:>4}] {}", msg.as_ref());
    }
}

fn mode(p: &std::path::Path) -> Option<u32> {
    std::fs::symlink_metadata(p).ok().map(|m| m.mode() & 0o777)
}

/// Every secret name the config refers to, with what needs it.
pub fn referenced_secrets(cfg: &Config) -> Vec<(String, String, bool)> {
    let mut out = vec![(
        crate::crypto::STORAGE_KEY_SECRET.to_owned(),
        "artifact/run-log encryption (created on first serve)".to_owned(),
        false,
    )];
    let p = &cfg.providers;
    if let Some(n) = p.nanogpt.as_ref().filter(|c| c.enabled) {
        out.push((n.api_key_secret.clone(), "nanogpt".into(), true));
    }
    if let Some(g) = p.cf_gateway.as_ref().filter(|c| c.enabled) {
        if let Some(t) = &g.aig_token_secret {
            out.push((t.clone(), "cf-gateway (cf-aig-authorization)".into(), false));
        }
        let mut provs: Vec<&str> = g
            .models
            .iter()
            .filter_map(|m| m.split_once('/').map(|x| x.0))
            .collect();
        provs.sort_unstable();
        provs.dedup();
        for pr in provs {
            out.push((
                format!("{}{pr}", g.provider_key_secret_prefix),
                format!("cf-gateway provider key for {pr} (optional with BYOK)"),
                false,
            ));
        }
    }
    if cfg.cloudflare.account_id.is_some() {
        out.push((
            cfg.cloudflare.api_token_secret.clone(),
            "cloudflare deploy".into(),
            true,
        ));
        if let Some(t) = &cfg.cloudflare.aig_token_secret {
            out.push((t.clone(), "cloudflare agent AI_GATEWAY_TOKEN".into(), true));
        }
    }
    for (name, def) in &cfg.mcp.servers {
        for s in def.secret_refs() {
            out.push((s.to_owned(), format!("MCP server {name}"), true));
        }
    }
    if let Some(g) = &cfg.telemetry.glitchtip
        && g.dsn.is_none()
        && let Some(s) = &g.dsn_secret
    {
        out.push((s.clone(), "GlitchTip DSN".into(), false));
    }
    out
}

fn cli_version(bin: &str) -> Result<String, String> {
    let path =
        crate::providers::cli::which(bin).ok_or_else(|| format!("{bin} not found on PATH"))?;
    let mut cmd = std::process::Command::new(&path);
    cmd.arg("--version")
        .env_clear()
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::null());
    for k in ["HOME", "PATH", "USER", "LANG"] {
        if let Some(v) = std::env::var_os(k) {
            cmd.env(k, v);
        }
    }
    let mut child = cmd.spawn().map_err(|e| e.to_string())?;
    let start = std::time::Instant::now();
    loop {
        match child.try_wait() {
            Ok(Some(_)) => break,
            Ok(None) if start.elapsed() > Duration::from_secs(10) => {
                let _ = child.kill();
                return Err("--version timed out".into());
            }
            Ok(None) => std::thread::sleep(Duration::from_millis(50)),
            Err(e) => return Err(e.to_string()),
        }
    }
    let out = child.wait_with_output().map_err(|e| e.to_string())?;
    let v = String::from_utf8_lossy(&out.stdout);
    Ok(format!(
        "{} ({})",
        crate::util::truncate(v.lines().next().unwrap_or("").trim(), 80),
        path.display()
    ))
}

pub async fn run(paths: &Paths) -> Report {
    let mut r = Report::default();
    let root = paths.root();
    match mode(root) {
        Some(0o700) => r.line("ok", format!("data dir {} (0700)", root.display())),
        Some(m) => r.line(
            "FAIL",
            format!("data dir {} has mode {m:o}; expected 0700", root.display()),
        ),
        None => r.line(
            "warn",
            format!(
                "data dir {} does not exist yet (created by serve)",
                root.display()
            ),
        ),
    }
    let cfg = match Config::load(&paths.config()) {
        Ok(c) => {
            if paths.config().exists() {
                r.line("ok", "agentd.toml parsed");
            } else {
                r.line("warn", "no agentd.toml; using defaults (no providers)");
            }
            c
        }
        Err(e) => {
            r.line("FAIL", format!("agentd.toml: {e:#}"));
            return r;
        }
    };
    match mode(&paths.token()) {
        Some(0o600) => r.line("ok", "agentd.token present (0600)"),
        Some(m) => r.line(
            "FAIL",
            format!("agentd.token has mode {m:o}; run `ko-agentd token rotate`"),
        ),
        None => r.line("warn", "agentd.token missing (created by serve)"),
    }
    let store = match cfg
        .secret_backend()
        .and_then(|k| SecretStore::open(k, paths))
    {
        Ok(s) => {
            r.line("ok", format!("secret store: {}", s.kind()));
            if s.kind() == "file" {
                r.line("warn", "file secret store is for development/testing only");
            }
            Some(s)
        }
        Err(e) => {
            r.line("FAIL", format!("secret store: {e:#}"));
            None
        }
    };
    if let Some(s) = &store {
        for (name, why, required) in referenced_secrets(&cfg) {
            match s.get(&name) {
                Ok(Some(_)) => r.line("ok", format!("secret {name} is set ({why})")),
                Ok(None) if required => r.line("FAIL", format!("secret {name} is not set ({why})")),
                Ok(None) => r.line("warn", format!("secret {name} is not set ({why})")),
                Err(e) => r.line(
                    "FAIL",
                    format!(
                        "secret {name}: {}",
                        crate::telemetry::scrub(&format!("{e:#}"))
                    ),
                ),
            }
        }
    }
    let p = &cfg.providers;
    if let Some(c) = p.claude_code.as_ref().filter(|c| c.enabled) {
        match cli_version(&c.binary) {
            Ok(v) => r.line("ok", format!("claude-code: {v}")),
            Err(e) => r.line("FAIL", format!("claude-code: {e}")),
        }
        if std::env::var_os("ANTHROPIC_API_KEY").is_some() {
            r.line("warn", "ANTHROPIC_API_KEY is set in this shell; agentd does not pass it to claude (subscription login is used)");
        }
    }
    if let Some(c) = p.codex.as_ref().filter(|c| c.enabled) {
        match cli_version(&c.binary) {
            Ok(v) => r.line("ok", format!("codex: {v}")),
            Err(e) => r.line("FAIL", format!("codex: {e}")),
        }
    }
    let models = crate::providers::list_models(&cfg);
    if models.is_empty() {
        r.line("warn", "no models configured");
    } else {
        r.line(
            "ok",
            format!(
                "{} model(s): {}",
                models.len(),
                models
                    .iter()
                    .map(|m| m.id.as_str())
                    .collect::<Vec<_>>()
                    .join(", ")
            ),
        );
    }
    match mode(&paths.socket()) {
        Some(0o600) => r.line("ok", "agentd.sock (0600)"),
        Some(m) => r.line("FAIL", format!("agentd.sock has mode {m:o}")),
        None => {}
    }
    let port = std::fs::read_to_string(paths.port_file())
        .ok()
        .and_then(|s| s.trim().parse::<u16>().ok())
        .unwrap_or(cfg.server.port);
    match tokio::time::timeout(
        Duration::from_secs(2),
        tokio::net::TcpStream::connect(("127.0.0.1", port)),
    )
    .await
    {
        Ok(Ok(_)) => r.line("ok", format!("something is listening on 127.0.0.1:{port}")),
        _ => r.line(
            "warn",
            format!("nothing listening on 127.0.0.1:{port} (daemon not running?)"),
        ),
    }
    r.line(
        "ok",
        format!(
            "telemetry: errors={} events={}",
            if cfg.telemetry.glitchtip.is_some() {
                "configured"
            } else {
                "off"
            },
            if cfg.telemetry.posthog.is_some() {
                "configured"
            } else {
                "off"
            },
        ),
    );
    if cfg.network.allow_loopback_http {
        r.line("warn", "network.allow_loopback_http is on (testing only)");
    }
    r
}
