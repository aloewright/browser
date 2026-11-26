// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

//! `<data>/agentd.toml`. Holds no secrets: secret values live in the
//! Keychain and are referenced here by name.

use std::collections::BTreeMap;
use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};

use anyhow::{Context, Result, bail};
use reqwest::Url;
use serde::{Deserialize, Serialize};

use crate::util::{valid_ident, valid_model_name, valid_secret_name};

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(deny_unknown_fields, default)]
pub struct Config {
    pub server: ServerConfig,
    pub secrets: SecretsConfig,
    pub network: NetworkConfig,
    pub providers: ProvidersConfig,
    pub mcp: McpConfig,
    pub workspaces: WorkspaceDefaults,
    pub runs: RunsConfig,
    pub cloudflare: CloudflareConfig,
    pub telemetry: TelemetryConfig,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields, default)]
pub struct ServerConfig {
    pub port: u16,
    pub unix_socket: bool,
    pub request_timeout_secs: u64,
    pub header_read_timeout_secs: u64,
    pub max_body_bytes: usize,
    pub max_chat_body_bytes: usize,
    pub max_artifact_bytes: usize,
}

impl Default for ServerConfig {
    fn default() -> Self {
        Self {
            port: crate::DEFAULT_PORT,
            unix_socket: true,
            request_timeout_secs: 60,
            header_read_timeout_secs: 10,
            max_body_bytes: 256 * 1024,
            max_chat_body_bytes: 8 * 1024 * 1024,
            max_artifact_bytes: 32 * 1024 * 1024,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum SecretBackendKind {
    /// macOS Keychain.
    Keychain,
    /// 0600 JSON file in the data dir. Tests/development only.
    File,
}

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(deny_unknown_fields, default)]
pub struct SecretsConfig {
    pub backend: Option<SecretBackendKind>,
}

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(deny_unknown_fields, default)]
pub struct NetworkConfig {
    /// Optional outbound proxy for provider/API calls (http, https, socks5h).
    pub proxy: Option<String>,
    /// Allows plain http to 127.0.0.1/localhost upstreams. Tests only.
    pub allow_loopback_http: bool,
}

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(deny_unknown_fields, default)]
pub struct ProvidersConfig {
    #[serde(rename = "claude-code")]
    pub claude_code: Option<ClaudeCodeConfig>,
    pub codex: Option<CodexConfig>,
    #[serde(rename = "cf-gateway")]
    pub cf_gateway: Option<CfGatewayConfig>,
    pub nanogpt: Option<NanoGptConfig>,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields, default)]
pub struct ClaudeCodeConfig {
    pub enabled: bool,
    /// Executable name (looked up on PATH) or absolute path.
    pub binary: String,
    pub models: Vec<String>,
    pub timeout_secs: u64,
    /// Passed as --permission-mode for agent runs.
    pub permission_mode: Option<String>,
    /// Passed as --allowedTools for agent runs.
    pub allowed_tools: Vec<String>,
    /// Extra env var names passed through to the CLI (values never logged).
    pub pass_env: Vec<String>,
}

impl Default for ClaudeCodeConfig {
    fn default() -> Self {
        Self {
            enabled: true,
            binary: "claude".into(),
            models: vec![],
            timeout_secs: 900,
            permission_mode: None,
            allowed_tools: vec![],
            pass_env: vec![],
        }
    }
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields, default)]
pub struct CodexConfig {
    pub enabled: bool,
    pub binary: String,
    pub models: Vec<String>,
    pub timeout_secs: u64,
    /// read-only | workspace-write | danger-full-access (agent runs).
    pub sandbox: String,
    /// Pass --ephemeral (no rollout files on disk).
    pub ephemeral: bool,
    pub pass_env: Vec<String>,
}

impl Default for CodexConfig {
    fn default() -> Self {
        Self {
            enabled: true,
            binary: "codex".into(),
            models: vec![],
            timeout_secs: 900,
            sandbox: "read-only".into(),
            ephemeral: true,
            pass_env: vec![],
        }
    }
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields, default)]
pub struct CfGatewayConfig {
    pub enabled: bool,
    pub account_id: String,
    pub gateway: String,
    pub base_url: String,
    /// "<provider>/<model>", exposed as "cf-gateway/<provider>/<model>".
    pub models: Vec<String>,
    /// Keychain name of the cf-aig-authorization token (authenticated gateway).
    pub aig_token_secret: Option<String>,
    /// Provider keys are looked up as "<prefix><provider>" (optional with BYOK).
    pub provider_key_secret_prefix: String,
    pub timeout_secs: u64,
}

impl Default for CfGatewayConfig {
    fn default() -> Self {
        Self {
            enabled: true,
            account_id: String::new(),
            gateway: String::new(),
            base_url: "https://gateway.ai.cloudflare.com/v1".into(),
            models: vec![],
            aig_token_secret: Some("cf-gateway-aig-token".into()),
            provider_key_secret_prefix: "cf-gateway-key-".into(),
            timeout_secs: 300,
        }
    }
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields, default)]
pub struct NanoGptConfig {
    pub enabled: bool,
    pub base_url: String,
    pub models: Vec<String>,
    pub api_key_secret: String,
    pub timeout_secs: u64,
}

impl Default for NanoGptConfig {
    fn default() -> Self {
        Self {
            enabled: true,
            base_url: "https://nano-gpt.com/api/v1".into(),
            models: vec![],
            api_key_secret: "nanogpt-api-key".into(),
            timeout_secs: 300,
        }
    }
}

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(deny_unknown_fields, default)]
pub struct McpConfig {
    /// Lets POST /v1/mcp/servers register stdio servers (arbitrary commands).
    pub allow_api_stdio_servers: bool,
    pub servers: BTreeMap<String, McpServerDef>,
}

/// An MCP server definition. Secret values are Keychain references.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "lowercase")]
pub enum McpServerDef {
    Stdio(StdioServer),
    /// Streamable HTTP.
    Http(HttpServer),
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct StdioServer {
    pub command: String,
    #[serde(default)]
    pub args: Vec<String>,
    /// Non-secret env.
    #[serde(default)]
    pub env: BTreeMap<String, String>,
    /// ENV_VAR -> Keychain secret name.
    #[serde(default)]
    pub env_secrets: BTreeMap<String, String>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct HttpServer {
    pub url: String,
    /// Non-secret headers.
    #[serde(default)]
    pub headers: BTreeMap<String, String>,
    /// Header -> Keychain secret name.
    #[serde(default)]
    pub header_secrets: BTreeMap<String, String>,
    /// Keychain secret sent as "Authorization: Bearer <value>".
    #[serde(default)]
    pub bearer_secret: Option<String>,
}

impl McpServerDef {
    pub fn secret_refs(&self) -> Vec<&str> {
        match self {
            McpServerDef::Stdio(s) => s.env_secrets.values().map(String::as_str).collect(),
            McpServerDef::Http(h) => h
                .header_secrets
                .values()
                .map(String::as_str)
                .chain(h.bearer_secret.as_deref())
                .collect(),
        }
    }

    pub fn validate(&self, name: &str, net: &NetworkConfig) -> Result<()> {
        if !valid_ident(name, 64) {
            bail!("invalid MCP server name {name:?}");
        }
        for s in self.secret_refs() {
            if !valid_secret_name(s) {
                bail!("MCP server {name}: invalid secret name {s:?}");
            }
        }
        match self {
            McpServerDef::Stdio(s) => {
                if s.command.is_empty() || s.command.starts_with('-') {
                    bail!("MCP server {name}: invalid command");
                }
                for k in s.env.keys().chain(s.env_secrets.keys()) {
                    if !valid_env_name(k) {
                        bail!("MCP server {name}: invalid env var name {k:?}");
                    }
                }
            }
            McpServerDef::Http(h) => {
                validate_upstream_url(&h.url, net).with_context(|| format!("MCP server {name}"))?;
                for k in h.headers.keys().chain(h.header_secrets.keys()) {
                    if !valid_ident(k, 64) {
                        bail!("MCP server {name}: invalid header name {k:?}");
                    }
                }
            }
        }
        Ok(())
    }
}

pub fn valid_env_name(s: &str) -> bool {
    !s.is_empty()
        && s.len() <= 128
        && !s.as_bytes()[0].is_ascii_digit()
        && s.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'_')
}

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(deny_unknown_fields, default)]
pub struct WorkspaceDefaults {
    pub default_model: Option<String>,
    /// Parent of per-workspace work dirs (default <data>/workspaces).
    pub workdir_root: Option<PathBuf>,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields, default)]
pub struct RunsConfig {
    pub max_concurrent: usize,
    pub timeout_secs: u64,
    /// Cap on persisted events per run.
    pub max_events: u64,
}

impl Default for RunsConfig {
    fn default() -> Self {
        Self {
            max_concurrent: 2,
            timeout_secs: 3600,
            max_events: 50_000,
        }
    }
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields, default)]
pub struct CloudflareConfig {
    pub account_id: Option<String>,
    pub api_base: String,
    pub api_token_secret: String,
    pub script_prefix: String,
    pub compatibility_date: String,
    pub access_team_domain: Option<String>,
    pub access_aud: Option<String>,
    pub ai_gateway_id: Option<String>,
    pub default_model: Option<String>,
    /// Uploaded as the AI_GATEWAY_TOKEN secret binding when set.
    pub aig_token_secret: Option<String>,
    /// Prebuilt worker module (dist/index.js of the template).
    pub bundle_path: Option<PathBuf>,
    /// ko/cloudflare/agent-template checkout; built at deploy time when no
    /// bundle_path is set.
    pub template_dir: Option<PathBuf>,
    /// Build command run in template_dir; must write dist/index.js.
    pub build_command: Vec<String>,
    pub workers_dev: bool,
    /// e.g. "https://{script}.agents.example.com"; overrides workers.dev URL.
    pub url_template: Option<String>,
}

impl Default for CloudflareConfig {
    fn default() -> Self {
        Self {
            account_id: None,
            api_base: "https://api.cloudflare.com/client/v4".into(),
            api_token_secret: "cloudflare-api-token".into(),
            script_prefix: "ko-agent-".into(),
            compatibility_date: "2026-10-01".into(),
            access_team_domain: None,
            access_aud: None,
            ai_gateway_id: None,
            default_model: None,
            aig_token_secret: None,
            bundle_path: None,
            template_dir: None,
            build_command: vec!["bun".into(), "run".into(), "build".into()],
            workers_dev: true,
            url_template: None,
        }
    }
}

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(deny_unknown_fields, default)]
pub struct TelemetryConfig {
    pub glitchtip: Option<GlitchTipConfig>,
    pub posthog: Option<PostHogConfig>,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields, default)]
pub struct GlitchTipConfig {
    /// Keychain name holding the DSN (preferred).
    pub dsn_secret: Option<String>,
    /// DSN in config (it is a write-only ingest key).
    pub dsn: Option<String>,
    pub environment: String,
}

impl Default for GlitchTipConfig {
    fn default() -> Self {
        Self {
            dsn_secret: Some("glitchtip-dsn".into()),
            dsn: None,
            environment: "production".into(),
        }
    }
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct PostHogConfig {
    pub host: String,
    pub project_key: String,
}

/// https only; http allowed for loopback when `allow_loopback_http`.
pub fn validate_upstream_url(raw: &str, net: &NetworkConfig) -> Result<Url> {
    let url = Url::parse(raw).with_context(|| format!("invalid URL {raw:?}"))?;
    if !url.username().is_empty() || url.password().is_some() {
        bail!("URLs must not embed credentials");
    }
    match url.scheme() {
        "https" => {}
        "http" if net.allow_loopback_http && is_loopback_host(&url) => {}
        s => bail!("URL scheme {s:?} not allowed (TLS required)"),
    }
    if url.host_str().is_none_or(str::is_empty) {
        bail!("URL has no host");
    }
    Ok(url)
}

fn is_loopback_host(url: &Url) -> bool {
    match url.host() {
        Some(url::Host::Domain(d)) => d.eq_ignore_ascii_case("localhost"),
        Some(url::Host::Ipv4(ip)) => ip.is_loopback(),
        Some(url::Host::Ipv6(ip)) => ip.is_loopback(),
        None => false,
    }
}

fn valid_hostname(s: &str) -> bool {
    !s.is_empty()
        && s.len() <= 253
        && s.bytes()
            .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'-'))
}

fn validate_models(provider: &str, models: &[String]) -> Result<()> {
    if models.is_empty() {
        bail!("providers.{provider}: `models` must list at least one model");
    }
    for m in models {
        if !valid_model_name(m) {
            bail!("providers.{provider}: invalid model name {m:?}");
        }
    }
    Ok(())
}

fn validate_binary(provider: &str, b: &str) -> Result<()> {
    if b.is_empty() || b.starts_with('-') || b.contains('\0') {
        bail!("providers.{provider}: invalid binary");
    }
    Ok(())
}

impl Config {
    pub fn from_toml(s: &str) -> Result<Self> {
        let cfg: Config = toml::from_str(s).context("parsing agentd.toml")?;
        cfg.validate()?;
        Ok(cfg)
    }

    /// Loads `<data>/agentd.toml`; a missing file means defaults.
    pub fn load(path: &Path) -> Result<Self> {
        match std::fs::symlink_metadata(path) {
            Ok(meta) => {
                if !meta.file_type().is_file() {
                    bail!("{} is not a regular file", path.display());
                }
                if meta.uid() != crate::paths::euid() {
                    bail!("{} is not owned by the current user", path.display());
                }
                if meta.mode() & 0o022 != 0 {
                    bail!(
                        "{} is group/world writable; run chmod 600 on it",
                        path.display()
                    );
                }
                let s = std::fs::read_to_string(path)
                    .with_context(|| format!("reading {}", path.display()))?;
                Self::from_toml(&s)
            }
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(Self::default()),
            Err(e) => Err(e).with_context(|| format!("stat {}", path.display())),
        }
    }

    pub fn validate(&self) -> Result<()> {
        let net = &self.network;
        if let Some(p) = &net.proxy {
            let u = Url::parse(p).context("network.proxy")?;
            if !matches!(u.scheme(), "http" | "https" | "socks5" | "socks5h") {
                bail!("network.proxy: unsupported scheme");
            }
        }
        if self.server.max_body_bytes == 0 || self.server.max_artifact_bytes == 0 {
            bail!("server: size limits must be > 0");
        }
        let p = &self.providers;
        if let Some(c) = &p.claude_code {
            validate_binary("claude-code", &c.binary)?;
            validate_models("claude-code", &c.models)?;
            if let Some(m) = &c.permission_mode
                && !matches!(
                    m.as_str(),
                    "default" | "acceptEdits" | "plan" | "bypassPermissions" | "dontAsk"
                )
            {
                bail!("providers.claude-code: invalid permission_mode {m:?}");
            }
            for t in &c.allowed_tools {
                if t.is_empty() || t.starts_with('-') || t.contains(',') {
                    bail!("providers.claude-code: invalid allowed_tools entry {t:?}");
                }
            }
            for e in &c.pass_env {
                if !valid_env_name(e) {
                    bail!("providers.claude-code: invalid pass_env entry {e:?}");
                }
            }
        }
        if let Some(c) = &p.codex {
            validate_binary("codex", &c.binary)?;
            validate_models("codex", &c.models)?;
            if !matches!(
                c.sandbox.as_str(),
                "read-only" | "workspace-write" | "danger-full-access"
            ) {
                bail!("providers.codex: invalid sandbox {:?}", c.sandbox);
            }
            for e in &c.pass_env {
                if !valid_env_name(e) {
                    bail!("providers.codex: invalid pass_env entry {e:?}");
                }
            }
        }
        if let Some(c) = &p.cf_gateway {
            validate_models("cf-gateway", &c.models)?;
            for m in &c.models {
                let (prov, model) = m.split_once('/').unwrap_or(("", ""));
                if !valid_ident(prov, 64) || model.is_empty() {
                    bail!("providers.cf-gateway: model {m:?} must be \"<provider>/<model>\"");
                }
            }
            if !valid_account_id(&c.account_id) {
                bail!("providers.cf-gateway: account_id must be a 32-char hex id");
            }
            if !valid_ident(&c.gateway, 64) {
                bail!("providers.cf-gateway: invalid gateway id");
            }
            validate_upstream_url(&c.base_url, net).context("providers.cf-gateway.base_url")?;
            if let Some(s) = &c.aig_token_secret
                && !valid_secret_name(s)
            {
                bail!("providers.cf-gateway: invalid aig_token_secret");
            }
            if !c.provider_key_secret_prefix.is_empty()
                && !valid_secret_name(&format!("{}x", c.provider_key_secret_prefix))
            {
                bail!("providers.cf-gateway: invalid provider_key_secret_prefix");
            }
        }
        if let Some(c) = &p.nanogpt {
            validate_models("nanogpt", &c.models)?;
            validate_upstream_url(&c.base_url, net).context("providers.nanogpt.base_url")?;
            if !valid_secret_name(&c.api_key_secret) {
                bail!("providers.nanogpt: invalid api_key_secret");
            }
        }
        for (name, def) in &self.mcp.servers {
            def.validate(name, net)?;
        }
        if self.runs.max_concurrent == 0 {
            bail!("runs.max_concurrent must be > 0");
        }
        let cf = &self.cloudflare;
        if let Some(a) = &cf.account_id
            && !valid_account_id(a)
        {
            bail!("cloudflare.account_id must be a 32-char hex id");
        }
        validate_upstream_url(&cf.api_base, net).context("cloudflare.api_base")?;
        if !valid_secret_name(&cf.api_token_secret) {
            bail!("cloudflare.api_token_secret is invalid");
        }
        if cf.script_prefix.is_empty()
            || cf.script_prefix.len() > 20
            || !cf
                .script_prefix
                .bytes()
                .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'-')
        {
            bail!("cloudflare.script_prefix must be [a-z0-9-]{{1,20}}");
        }
        if let Some(d) = &cf.access_team_domain
            && !valid_hostname(d)
        {
            bail!("cloudflare.access_team_domain must be a hostname");
        }
        if let Some(a) = &cf.access_aud
            && !(a.len() <= 128 && a.bytes().all(|b| b.is_ascii_alphanumeric()))
        {
            bail!("cloudflare.access_aud is invalid");
        }
        if let Some(g) = &cf.ai_gateway_id
            && !valid_ident(g, 64)
        {
            bail!("cloudflare.ai_gateway_id is invalid");
        }
        if let Some(s) = &cf.aig_token_secret
            && !valid_secret_name(s)
        {
            bail!("cloudflare.aig_token_secret is invalid");
        }
        if cf.build_command.is_empty() || cf.build_command[0].starts_with('-') {
            bail!("cloudflare.build_command must name a program");
        }
        if let Some(t) = &cf.url_template {
            validate_upstream_url(&t.replace("{script}", "x"), net)
                .context("cloudflare.url_template")?;
        }
        if let Some(g) = &self.telemetry.glitchtip {
            if let Some(s) = &g.dsn_secret
                && !valid_secret_name(s)
            {
                bail!("telemetry.glitchtip.dsn_secret is invalid");
            }
            if let Some(d) = &g.dsn {
                crate::telemetry::Dsn::parse(d, net).context("telemetry.glitchtip.dsn")?;
            }
        }
        if let Some(ph) = &self.telemetry.posthog {
            validate_upstream_url(&ph.host, net).context("telemetry.posthog.host")?;
            if ph.project_key.is_empty() || ph.project_key.len() > 128 {
                bail!("telemetry.posthog.project_key is invalid");
            }
        }
        Ok(())
    }

    pub fn secret_backend(&self) -> Result<SecretBackendKind> {
        match self.secrets.backend {
            Some(b) => Ok(b),
            None if cfg!(target_os = "macos") => Ok(SecretBackendKind::Keychain),
            None => bail!(
                "no secure secret store on this platform; set [secrets] backend = \"file\" \
                 (stores secrets in a 0600 file; development/testing only)"
            ),
        }
    }
}

pub fn valid_account_id(s: &str) -> bool {
    s.len() == 32 && s.bytes().all(|b| b.is_ascii_hexdigit())
}

#[cfg(test)]
mod tests {
    use super::*;

    const SAMPLE: &str = r#"
[server]
port = 47999

[secrets]
backend = "file"

[providers.claude-code]
models = ["sonnet", "opus"]
permission_mode = "acceptEdits"

[providers.codex]
models = ["gpt-5-codex"]
sandbox = "workspace-write"

[providers.cf-gateway]
account_id = "0123456789abcdef0123456789abcdef"
gateway = "ko"
models = ["openai/gpt-4o-mini"]

[providers.nanogpt]
models = ["chatgpt-4o-latest"]

[mcp.servers.fs]
type = "stdio"
command = "npx"
args = ["-y", "@modelcontextprotocol/server-filesystem", "/tmp"]
env_secrets = { GITHUB_TOKEN = "mcp-github-token" }

[mcp.servers.docs]
type = "http"
url = "https://mcp.example.com/mcp"
bearer_secret = "mcp-docs-token"

[cloudflare]
account_id = "0123456789abcdef0123456789abcdef"
access_team_domain = "example.cloudflareaccess.com"
access_aud = "abc123"

[telemetry.posthog]
host = "https://posthog.example.com"
project_key = "phc_placeholder"
"#;

    #[test]
    fn parses_sample() {
        let c = Config::from_toml(SAMPLE).unwrap();
        assert_eq!(c.server.port, 47999);
        assert_eq!(c.secret_backend().unwrap(), SecretBackendKind::File);
        assert_eq!(c.providers.claude_code.as_ref().unwrap().binary, "claude");
        assert_eq!(
            c.providers.codex.as_ref().unwrap().sandbox,
            "workspace-write"
        );
        assert_eq!(c.mcp.servers.len(), 2);
        assert!(matches!(c.mcp.servers["fs"], McpServerDef::Stdio(_)));
        assert_eq!(c.mcp.servers["docs"].secret_refs(), vec!["mcp-docs-token"]);
    }

    #[test]
    fn example_config_is_valid() {
        let c = Config::from_toml(include_str!("../agentd.example.toml")).unwrap();
        assert_eq!(crate::providers::list_models(&c).len(), 6);
        assert!(c.cloudflare.template_dir.is_some());
    }

    #[test]
    fn defaults_when_empty() {
        let c = Config::from_toml("").unwrap();
        assert_eq!(c.server.port, crate::DEFAULT_PORT);
        assert!(c.providers.claude_code.is_none());
    }

    #[test]
    fn rejects_unknown_keys_and_inline_secrets() {
        assert!(
            Config::from_toml("[providers.nanogpt]\nmodels=[\"a\"]\napi_key=\"sk-x\"").is_err()
        );
        assert!(Config::from_toml("[bogus]\nx=1").is_err());
        let bad_mcp = "[mcp.servers.x]\ntype=\"http\"\nurl=\"https://a.example\"\ntoken=\"t\"";
        assert!(Config::from_toml(bad_mcp).is_err());
    }

    #[test]
    fn rejects_plain_http_and_bad_values() {
        let http = "[providers.nanogpt]\nmodels=[\"a\"]\nbase_url=\"http://nano-gpt.com/api/v1\"";
        assert!(Config::from_toml(http).is_err());
        let loopback = "[network]\nallow_loopback_http=true\n[providers.nanogpt]\nmodels=[\"a\"]\nbase_url=\"http://127.0.0.1:9/v1\"";
        assert!(Config::from_toml(loopback).is_ok());
        let creds =
            "[providers.nanogpt]\nmodels=[\"a\"]\nbase_url=\"https://u:p@nano-gpt.com/api/v1\"";
        assert!(Config::from_toml(creds).is_err());
        assert!(Config::from_toml("[providers.claude-code]\nmodels=[\"--evil\"]").is_err());
        assert!(Config::from_toml("[providers.claude-code]\nmodels=[]").is_err());
        assert!(Config::from_toml("[providers.codex]\nmodels=[\"m\"]\nsandbox=\"yolo\"").is_err());
        assert!(
            Config::from_toml(
                "[providers.cf-gateway]\naccount_id=\"nothex\"\ngateway=\"g\"\nmodels=[\"openai/x\"]"
            )
            .is_err()
        );
    }

    #[test]
    fn load_checks_permissions() {
        use std::os::unix::fs::PermissionsExt;
        let t = tempfile::tempdir().unwrap();
        let p = t.path().join("agentd.toml");
        assert!(Config::load(&p).unwrap().providers.nanogpt.is_none());
        std::fs::write(&p, "[server]\nport = 1234\n").unwrap();
        std::fs::set_permissions(&p, std::fs::Permissions::from_mode(0o666)).unwrap();
        assert!(Config::load(&p).is_err());
        std::fs::set_permissions(&p, std::fs::Permissions::from_mode(0o600)).unwrap();
        assert_eq!(Config::load(&p).unwrap().server.port, 1234);
    }
}
