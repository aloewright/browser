// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

//! MCP server registry (config + API-added) and per-run configs for the
//! claude and codex CLIs.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use anyhow::{Context, Result, bail};
use serde::Serialize;
use serde_json::{Map, Value, json};
use zeroize::Zeroizing;

use crate::config::{Config, McpServerDef};
use crate::db::{self, Db};
use crate::providers::cli::BASE_ENV;
use crate::secrets::SecretStore;

#[derive(Debug, Clone, Serialize)]
pub struct SecretRefStatus {
    pub name: String,
    pub set: bool,
}

#[derive(Debug, Clone, Serialize)]
pub struct McpServerView {
    pub name: String,
    #[serde(rename = "type")]
    pub kind: &'static str,
    pub source: &'static str,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub command: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub args: Option<Vec<String>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub url: Option<String>,
    /// Names only; values are never returned.
    pub env_keys: Vec<String>,
    pub header_keys: Vec<String>,
    pub secret_refs: Vec<SecretRefStatus>,
}

/// All servers: config entries first (read-only), then API-added ones.
pub async fn list(cfg: &Config, db: &Db) -> Result<Vec<(String, McpServerDef, &'static str)>> {
    let mut out: Vec<(String, McpServerDef, &'static str)> = cfg
        .mcp
        .servers
        .iter()
        .map(|(k, v)| (k.clone(), v.clone(), "config"))
        .collect();
    let rows = db.call(|c| db::list_mcp_servers(c)).await?;
    for (name, json) in rows {
        if cfg.mcp.servers.contains_key(&name) {
            continue;
        }
        match serde_json::from_str::<McpServerDef>(&json) {
            Ok(def) => out.push((name, def, "api")),
            Err(_) => tracing::warn!(server = %name, "skipping unreadable MCP server definition"),
        }
    }
    Ok(out)
}

pub fn view(
    name: &str,
    def: &McpServerDef,
    source: &'static str,
    secrets: &SecretStore,
) -> McpServerView {
    let secret_refs = def
        .secret_refs()
        .into_iter()
        .map(|s| SecretRefStatus {
            name: s.to_owned(),
            set: secrets.get(s).ok().flatten().is_some(),
        })
        .collect();
    match def {
        McpServerDef::Stdio(s) => McpServerView {
            name: name.to_owned(),
            kind: "stdio",
            source,
            command: Some(s.command.clone()),
            args: Some(s.args.clone()),
            url: None,
            env_keys: s.env.keys().chain(s.env_secrets.keys()).cloned().collect(),
            header_keys: vec![],
            secret_refs,
        },
        McpServerDef::Http(h) => McpServerView {
            name: name.to_owned(),
            kind: "http",
            source,
            command: None,
            args: None,
            url: Some(h.url.clone()),
            env_keys: vec![],
            header_keys: h
                .headers
                .keys()
                .chain(h.header_secrets.keys())
                .cloned()
                .collect(),
            secret_refs,
        },
    }
}

/// Picks the named servers; unknown names are an error.
pub async fn select(
    cfg: &Config,
    db: &Db,
    names: &[String],
) -> Result<Vec<(String, McpServerDef)>> {
    let all = list(cfg, db).await?;
    names
        .iter()
        .map(|n| {
            all.iter()
                .find(|(name, _, _)| name == n)
                .map(|(name, def, _)| (name.clone(), def.clone()))
                .with_context(|| format!("unknown MCP server {n:?}"))
        })
        .collect()
}

fn resolve_secret(secrets: &SecretStore, name: &str) -> Result<Zeroizing<String>> {
    secrets.require(name)
}

/// `--mcp-config` JSON for claude, with secrets resolved (written 0600).
pub fn claude_config(
    servers: &[(String, McpServerDef)],
    secrets: &SecretStore,
) -> Result<Zeroizing<String>> {
    let mut map = Map::new();
    for (name, def) in servers {
        let v = match def {
            McpServerDef::Stdio(s) => {
                let mut env = Map::new();
                for (k, v) in &s.env {
                    env.insert(k.clone(), json!(v));
                }
                for (k, secret) in &s.env_secrets {
                    env.insert(k.clone(), json!(resolve_secret(secrets, secret)?.as_str()));
                }
                json!({"type": "stdio", "command": s.command, "args": s.args, "env": env})
            }
            McpServerDef::Http(h) => {
                let mut headers = Map::new();
                for (k, v) in &h.headers {
                    headers.insert(k.clone(), json!(v));
                }
                for (k, secret) in &h.header_secrets {
                    headers.insert(k.clone(), json!(resolve_secret(secrets, secret)?.as_str()));
                }
                if let Some(b) = &h.bearer_secret {
                    headers.insert(
                        "Authorization".into(),
                        json!(format!("Bearer {}", resolve_secret(secrets, b)?.as_str())),
                    );
                }
                json!({"type": "http", "url": h.url, "headers": headers})
            }
        };
        map.insert(name.clone(), v);
    }
    let out = Zeroizing::new(serde_json::to_string_pretty(&json!({ "mcpServers": map }))?);
    Ok(out)
}

/// Writes the claude MCP config into a private run dir.
pub fn write_claude_config(dir: &Path, json: &str) -> Result<PathBuf> {
    let p = dir.join("mcp.json");
    if !crate::paths::create_private_file_exclusive(&p, json.as_bytes())? {
        bail!("MCP config already exists");
    }
    Ok(p)
}

fn toml_str(s: &str) -> String {
    toml::Value::String(s.to_owned()).to_string()
}

fn toml_str_array<'a>(items: impl IntoIterator<Item = &'a String>) -> String {
    let parts: Vec<String> = items.into_iter().map(|s| toml_str(s)).collect();
    format!("[{}]", parts.join(", "))
}

fn toml_inline_table(m: &BTreeMap<String, String>) -> String {
    let parts: Vec<String> = m
        .iter()
        .map(|(k, v)| format!("{} = {}", toml_str(k), toml_str(v)))
        .collect();
    format!("{{ {} }}", parts.join(", "))
}

fn env_suffix(name: &str) -> String {
    name.to_ascii_uppercase().replace('-', "_")
}

/// Codex has no MCP config-file flag, so servers become `-c` overrides that
/// contain no secret values; secrets reach the MCP servers through env vars
/// referenced by name (env_vars, bearer_token_env_var, env_http_headers).
pub struct CodexMcp {
    pub args: Vec<String>,
    pub env: Vec<(String, Zeroizing<String>)>,
}

pub fn codex_overrides(
    servers: &[(String, McpServerDef)],
    secrets: &SecretStore,
) -> Result<CodexMcp> {
    let mut args = Vec::new();
    let mut env: Vec<(String, Zeroizing<String>)> = Vec::new();
    let mut set = |k: String, v: String| {
        args.push("-c".to_owned());
        args.push(format!("{k}={v}"));
    };
    for (name, def) in servers {
        let base = format!("mcp_servers.{name}");
        match def {
            McpServerDef::Stdio(s) => {
                set(format!("{base}.command"), toml_str(&s.command));
                set(format!("{base}.args"), toml_str_array(&s.args));
                if !s.env.is_empty() {
                    set(format!("{base}.env"), toml_inline_table(&s.env));
                }
                if !s.env_secrets.is_empty() {
                    for (k, secret) in &s.env_secrets {
                        if BASE_ENV.contains(&k.as_str()) || k == "TMPDIR" {
                            bail!("MCP server {name}: env var {k} cannot carry a secret");
                        }
                        env.push((k.clone(), resolve_secret(secrets, secret)?));
                    }
                    set(
                        format!("{base}.env_vars"),
                        toml_str_array(s.env_secrets.keys()),
                    );
                }
            }
            McpServerDef::Http(h) => {
                set(format!("{base}.url"), toml_str(&h.url));
                if !h.headers.is_empty() {
                    set(
                        format!("{base}.http_headers"),
                        toml_inline_table(&h.headers),
                    );
                }
                if let Some(b) = &h.bearer_secret {
                    let var = format!("KO_MCP_{}_BEARER", env_suffix(name));
                    env.push((var.clone(), resolve_secret(secrets, b)?));
                    set(format!("{base}.bearer_token_env_var"), toml_str(&var));
                }
                if !h.header_secrets.is_empty() {
                    let mut refs = BTreeMap::new();
                    for (i, (header, secret)) in h.header_secrets.iter().enumerate() {
                        let var = format!("KO_MCP_{}_H{i}", env_suffix(name));
                        env.push((var.clone(), resolve_secret(secrets, secret)?));
                        refs.insert(header.clone(), var);
                    }
                    set(format!("{base}.env_http_headers"), toml_inline_table(&refs));
                }
            }
        }
    }
    if !env.is_empty() {
        // Keep MCP secrets out of the shell tool's environment.
        let names: Vec<String> = env.iter().map(|(k, _)| k.clone()).collect();
        args.push("-c".into());
        args.push(format!(
            "shell_environment_policy.exclude={}",
            toml_str_array(&names)
        ));
    }
    Ok(CodexMcp { args, env })
}

/// Validated definition from a POST body (secrets by reference only).
pub fn parse_api_definition(cfg: &Config, name: &str, body: Value) -> Result<McpServerDef> {
    let def: McpServerDef =
        serde_json::from_value(body).context("invalid MCP server definition")?;
    def.validate(name, &cfg.network)?;
    if matches!(def, McpServerDef::Stdio(_)) && !cfg.mcp.allow_api_stdio_servers {
        bail!(
            "stdio servers can only be added in agentd.toml (mcp.allow_api_stdio_servers = false)"
        );
    }
    Ok(def)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::config::{HttpServer, SecretBackendKind, StdioServer};
    use crate::paths::Paths;

    fn store() -> (tempfile::TempDir, SecretStore) {
        let t = tempfile::tempdir().unwrap();
        let s = SecretStore::open(SecretBackendKind::File, &Paths::new(t.path())).unwrap();
        s.set("gh-token", "ghp_secretvalue123").unwrap();
        s.set("docs-token", "docs-secret-456").unwrap();
        (t, s)
    }

    fn servers() -> Vec<(String, McpServerDef)> {
        vec![
            (
                "github".into(),
                McpServerDef::Stdio(StdioServer {
                    command: "npx".into(),
                    args: vec!["-y".into(), "server-github".into()],
                    env: BTreeMap::from([("LOG_LEVEL".into(), "info".into())]),
                    env_secrets: BTreeMap::from([("GITHUB_TOKEN".into(), "gh-token".into())]),
                }),
            ),
            (
                "docs-api".into(),
                McpServerDef::Http(HttpServer {
                    url: "https://mcp.example.com/mcp".into(),
                    headers: BTreeMap::from([("X-Client".into(), "ko".into())]),
                    header_secrets: BTreeMap::new(),
                    bearer_secret: Some("docs-token".into()),
                }),
            ),
        ]
    }

    #[test]
    fn claude_json_resolves_secrets() {
        let (_t, s) = store();
        let j: Value = serde_json::from_str(&claude_config(&servers(), &s).unwrap()).unwrap();
        let gh = &j["mcpServers"]["github"];
        assert_eq!(gh["type"], "stdio");
        assert_eq!(gh["env"]["GITHUB_TOKEN"], "ghp_secretvalue123");
        assert_eq!(gh["env"]["LOG_LEVEL"], "info");
        let docs = &j["mcpServers"]["docs-api"];
        assert_eq!(docs["type"], "http");
        assert_eq!(docs["headers"]["Authorization"], "Bearer docs-secret-456");
    }

    #[test]
    fn codex_overrides_keep_secrets_off_argv() {
        let (_t, s) = store();
        let o = codex_overrides(&servers(), &s).unwrap();
        let argv = o.args.join(" ");
        assert!(!argv.contains("ghp_secretvalue123"));
        assert!(!argv.contains("docs-secret-456"));
        assert!(
            o.args
                .contains(&"mcp_servers.github.command=\"npx\"".to_string())
        );
        assert!(
            o.args
                .contains(&"mcp_servers.github.env_vars=[\"GITHUB_TOKEN\"]".to_string())
        );
        assert!(o.args.contains(
            &"mcp_servers.docs-api.bearer_token_env_var=\"KO_MCP_DOCS_API_BEARER\"".to_string()
        ));
        assert!(argv.contains("shell_environment_policy.exclude="));
        let env: BTreeMap<_, _> = o
            .env
            .iter()
            .map(|(k, v)| (k.clone(), v.to_string()))
            .collect();
        assert_eq!(env["GITHUB_TOKEN"], "ghp_secretvalue123");
        assert_eq!(env["KO_MCP_DOCS_API_BEARER"], "docs-secret-456");
        // Every override value parses as TOML.
        for a in o.args.iter().filter(|a| a.contains('=')) {
            let (k, v) = a.split_once('=').unwrap();
            toml::from_str::<toml::Table>(&format!("x = {v}")).unwrap_or_else(|_| panic!("{k}"));
        }
    }

    #[test]
    fn missing_secret_fails_closed() {
        let (_t, s) = store();
        s.delete("gh-token").unwrap();
        assert!(claude_config(&servers(), &s).is_err());
        assert!(codex_overrides(&servers(), &s).is_err());
    }

    #[test]
    fn api_definitions() {
        let cfg = Config::default();
        let http = json!({"type": "http", "url": "https://mcp.example.com", "bearer_secret": "t"});
        assert!(parse_api_definition(&cfg, "x", http).is_ok());
        let stdio = json!({"type": "stdio", "command": "sh"});
        assert!(parse_api_definition(&cfg, "x", stdio).is_err());
        let inline = json!({"type": "http", "url": "https://a.example", "token": "x"});
        assert!(parse_api_definition(&cfg, "x", inline).is_err());
        let plain = json!({"type": "http", "url": "http://a.example"});
        assert!(parse_api_definition(&cfg, "x", plain).is_err());
        let bad_name = json!({"type": "http", "url": "https://a.example"});
        assert!(parse_api_definition(&cfg, "a.b", bad_name).is_err());
    }
}
