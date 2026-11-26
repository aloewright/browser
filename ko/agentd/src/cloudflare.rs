// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

//! Deploys the per-workspace Cloudflare agent worker
//! (ko/cloudflare/agent-template) through the Cloudflare API.

use std::path::Path;
use std::process::Stdio;
use std::time::Duration;

use reqwest::header::{AUTHORIZATION, HeaderValue};
use serde_json::{Value, json};
use zeroize::Zeroizing;

use crate::config::{CloudflareConfig, validate_upstream_url};
use crate::db::{CloudflareAgentInfo, Workspace};
use crate::error::{ApiError, ApiResult};
use crate::server::Core;

/// Durable Object class exported by the template; also the binding name.
pub const AGENT_CLASS: &str = "KoAgent";
pub const MIGRATION_TAG: &str = "v1";
const MAX_BUNDLE: u64 = 10 * 1024 * 1024;
const BUILD_TIMEOUT: Duration = Duration::from_secs(300);

/// Worker bundle embedded at build time from assets/ (see build.rs).
pub fn embedded_bundle() -> Option<&'static str> {
    let s: &'static str = include_str!(concat!(env!("OUT_DIR"), "/ko-agent-worker.js"));
    (!s.is_empty()).then_some(s)
}

async fn read_bundle(p: &Path) -> ApiResult<Zeroizing<String>> {
    let meta = tokio::fs::metadata(p)
        .await
        .map_err(|_| ApiError::unavailable(format!("worker bundle {} not found", p.display())))?;
    if meta.len() > MAX_BUNDLE {
        return Err(ApiError::unavailable("worker bundle is too large"));
    }
    Ok(Zeroizing::new(tokio::fs::read_to_string(p).await.map_err(
        |_| ApiError::unavailable("worker bundle is not readable"),
    )?))
}

/// Runs the template's build in `template_dir` (minimal env, no secrets).
async fn build_bundle(dir: &Path, command: &[String]) -> ApiResult<Zeroizing<String>> {
    let (prog, args) = command
        .split_first()
        .ok_or_else(|| ApiError::unavailable("cloudflare.build_command is empty"))?;
    let program = crate::providers::cli::which(prog)
        .ok_or_else(|| ApiError::unavailable(format!("{prog} not found on PATH")))?;
    let mut cmd = tokio::process::Command::new(program);
    cmd.args(args)
        .current_dir(dir)
        .env_clear()
        .env("WRANGLER_SEND_METRICS", "false")
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::piped())
        .kill_on_drop(true);
    for k in crate::providers::cli::BASE_ENV {
        if let Some(v) = std::env::var_os(k) {
            cmd.env(k, v);
        }
    }
    let child = cmd
        .spawn()
        .map_err(|e| ApiError::unavailable(format!("starting worker build: {e}")))?;
    let out = tokio::time::timeout(BUILD_TIMEOUT, child.wait_with_output())
        .await
        .map_err(|_| ApiError::unavailable("worker build timed out"))?
        .map_err(|e| ApiError::unavailable(format!("worker build: {e}")))?;
    if !out.status.success() {
        let err = String::from_utf8_lossy(&out.stderr);
        let tail = crate::util::truncate(err.trim(), 1024);
        return Err(ApiError::unavailable(crate::telemetry::scrub(&format!(
            "worker build failed: {tail}"
        ))));
    }
    read_bundle(&dir.join("dist").join("index.js")).await
}

/// bundle_path, else a deploy-time build of template_dir, else the bundle
/// embedded at compile time.
async fn load_bundle(cf: &CloudflareConfig) -> ApiResult<Zeroizing<String>> {
    if let Some(p) = &cf.bundle_path {
        return read_bundle(p).await;
    }
    if let Some(dir) = &cf.template_dir {
        return build_bundle(dir, &cf.build_command).await;
    }
    embedded_bundle()
        .map(|s| Zeroizing::new(s.to_owned()))
        .ok_or_else(|| {
            ApiError::unavailable(
                "no worker bundle: set cloudflare.template_dir (built at deploy time) or bundle_path",
            )
        })
}

pub fn script_name(prefix: &str, workspace_id: &str) -> String {
    format!("{prefix}{}", workspace_id.replace('-', ""))
}

fn cf_error(context: &str, v: &Value) -> ApiError {
    let errs: Vec<String> = v["errors"]
        .as_array()
        .map(|a| {
            a.iter()
                .map(|e| {
                    format!(
                        "{} {}",
                        e["code"].as_i64().unwrap_or(0),
                        crate::util::truncate(e["message"].as_str().unwrap_or(""), 200)
                    )
                })
                .collect()
        })
        .unwrap_or_default();
    ApiError::provider(crate::telemetry::scrub(&format!(
        "cloudflare API {context} failed: {}",
        if errs.is_empty() {
            "unknown error".into()
        } else {
            errs.join("; ")
        }
    )))
}

struct Api<'a> {
    http: &'a reqwest::Client,
    base: String,
    auth: HeaderValue,
}

impl Api<'_> {
    async fn call(&self, context: &str, rb: reqwest::RequestBuilder) -> ApiResult<Value> {
        let resp = rb
            .header(AUTHORIZATION, self.auth.clone())
            .send()
            .await
            .map_err(|e| {
                ApiError::provider(crate::telemetry::scrub(&format!(
                    "cloudflare API {context}: {}",
                    e.without_url()
                )))
            })?;
        let status = resp.status();
        let bytes = resp
            .bytes()
            .await
            .map_err(|_| ApiError::provider(format!("cloudflare API {context}: read failed")))?;
        let v: Value = serde_json::from_slice(&bytes).unwrap_or(Value::Null);
        if !status.is_success() || v["success"].as_bool() != Some(true) {
            return Err(cf_error(context, &v));
        }
        Ok(v)
    }

    fn url(&self, path: &str) -> String {
        format!("{}{path}", self.base)
    }
}

fn required<'a>(v: &'a Option<String>, what: &str) -> ApiResult<&'a str> {
    v.as_deref()
        .filter(|s| !s.is_empty())
        .ok_or_else(|| ApiError::unavailable(format!("cloudflare.{what} is not configured")))
}

/// Uploads (or updates) the workspace's worker and returns its URL.
pub async fn deploy(core: &Core, ws: &Workspace) -> ApiResult<CloudflareAgentInfo> {
    let cf = &core.cfg.cloudflare;
    let account = required(&cf.account_id, "account_id")?;
    let team_domain = required(&cf.access_team_domain, "access_team_domain")?;
    let aud = required(&cf.access_aud, "access_aud")?;
    let gateway = cf
        .ai_gateway_id
        .clone()
        .or_else(|| {
            core.cfg
                .providers
                .cf_gateway
                .as_ref()
                .map(|g| g.gateway.clone())
        })
        .filter(|g| !g.is_empty())
        .ok_or_else(|| ApiError::unavailable("cloudflare.ai_gateway_id is not configured"))?;
    let default_model = cf
        .default_model
        .clone()
        .unwrap_or_else(|| "workers-ai/@cf/meta/llama-3.1-8b-instruct".into());

    let bundle = load_bundle(cf).await?;

    let token = core
        .secrets
        .require_async(&cf.api_token_secret)
        .await
        .map_err(|e| ApiError::unavailable(format!("{e:#}")))?;
    let mut auth = HeaderValue::from_str(&format!("Bearer {}", token.as_str()))
        .map_err(|_| ApiError::unavailable("invalid Cloudflare API token"))?;
    auth.set_sensitive(true);
    let base = validate_upstream_url(&cf.api_base, &core.cfg.network)
        .map_err(|e| ApiError::unavailable(format!("{e:#}")))?
        .as_str()
        .trim_end_matches('/')
        .to_owned();
    let api = Api {
        http: &core.http,
        base,
        auth,
    };
    let name = script_name(&cf.script_prefix, &ws.id);
    let acct = format!("/accounts/{account}/workers");

    // Durable Object migrations must only be sent once per script.
    let scripts = api
        .call(
            "list scripts",
            api.http.get(api.url(&format!("{acct}/scripts"))),
        )
        .await?;
    let current_tag = scripts["result"]
        .as_array()
        .and_then(|a| a.iter().find(|s| s["id"].as_str() == Some(name.as_str())))
        .and_then(|s| s["migration_tag"].as_str())
        .unwrap_or("")
        .to_owned();

    let mut bindings = vec![
        json!({"type": "durable_object_namespace", "name": AGENT_CLASS, "class_name": AGENT_CLASS}),
        json!({"type": "plain_text", "name": "ACCESS_TEAM_DOMAIN", "text": team_domain}),
        json!({"type": "plain_text", "name": "ACCESS_AUD", "text": aud}),
        json!({"type": "plain_text", "name": "AI_GATEWAY_ACCOUNT_ID", "text": account}),
        json!({"type": "plain_text", "name": "AI_GATEWAY_ID", "text": gateway}),
        json!({"type": "plain_text", "name": "DEFAULT_MODEL", "text": default_model}),
        json!({"type": "plain_text", "name": "KO_WORKSPACE_ID", "text": ws.id}),
    ];
    if let Some(sname) = &cf.aig_token_secret {
        let tok = core
            .secrets
            .require_async(sname)
            .await
            .map_err(|e| ApiError::unavailable(format!("{e:#}")))?;
        bindings
            .push(json!({"type": "secret_text", "name": "AI_GATEWAY_TOKEN", "text": tok.as_str()}));
    }
    let mut metadata = json!({
        "main_module": "index.js",
        "compatibility_date": cf.compatibility_date,
        "compatibility_flags": ["nodejs_compat"],
        "bindings": bindings,
    });
    match current_tag.as_str() {
        "" => {
            metadata["migrations"] =
                json!({"new_tag": MIGRATION_TAG, "new_sqlite_classes": [AGENT_CLASS]});
        }
        MIGRATION_TAG => {}
        other => {
            return Err(ApiError::conflict(format!(
                "worker {name} has unexpected migration tag {:?}",
                crate::util::truncate(other, 40)
            )));
        }
    }
    let metadata =
        Zeroizing::new(serde_json::to_string(&metadata).map_err(|e| ApiError::internal(e.into()))?);
    let form = reqwest::multipart::Form::new()
        .part(
            "metadata",
            reqwest::multipart::Part::text(metadata.to_string())
                .mime_str("application/json")
                .map_err(|e| ApiError::internal(e.into()))?,
        )
        .part(
            "index.js",
            reqwest::multipart::Part::text(bundle.to_string())
                .file_name("index.js")
                .mime_str("application/javascript+module")
                .map_err(|e| ApiError::internal(e.into()))?,
        );
    api.call(
        "upload script",
        api.http
            .put(api.url(&format!("{acct}/scripts/{name}")))
            .multipart(form),
    )
    .await?;

    let url = if let Some(t) = &cf.url_template {
        t.replace("{script}", &name)
    } else if cf.workers_dev {
        api.call(
            "enable workers.dev",
            api.http
                .post(api.url(&format!("{acct}/scripts/{name}/subdomain")))
                .json(&json!({"enabled": true, "previews_enabled": false})),
        )
        .await?;
        let sub = api
            .call(
                "get subdomain",
                api.http.get(api.url(&format!("{acct}/subdomain"))),
            )
            .await?;
        let sub = sub["result"]["subdomain"]
            .as_str()
            .filter(|s| s.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-') && !s.is_empty())
            .ok_or_else(|| {
                ApiError::provider("cloudflare API returned no workers.dev subdomain")
            })?;
        format!("https://{name}.{sub}.workers.dev")
    } else {
        return Err(ApiError::unavailable(
            "set cloudflare.url_template or enable cloudflare.workers_dev",
        ));
    };
    Ok(CloudflareAgentInfo {
        script_name: name,
        url,
        deployed_at: crate::util::now_ms(),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn names() {
        assert_eq!(
            script_name("ko-agent-", "0b0c6c1e-6a52-4c8e-9b1a-1f2e3d4c5b6a"),
            "ko-agent-0b0c6c1e6a524c8e9b1a1f2e3d4c5b6a"
        );
    }
}
