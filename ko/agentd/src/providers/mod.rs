// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

//! Model providers. Ids: "claude-code/<m>", "codex/<m>",
//! "cf-gateway/<provider>/<m>", "nanogpt/<m>".

pub mod claude;
pub mod cli;
pub mod codex;
pub mod http;
pub mod openai;
pub mod prompt;

use std::sync::Arc;

use serde::Serialize;
use serde_json::Value;

use crate::config::{Config, DEFAULT_CONTEXT_LENGTH, MAX_DISPLAY_NAME, ModelInfoConfig};
use crate::error::ApiError;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ModelRef {
    ClaudeCode { model: String },
    Codex { model: String },
    CfGateway { provider: String, model: String },
    NanoGpt { model: String },
}

impl ModelRef {
    pub fn parse(id: &str) -> Option<Self> {
        let (prefix, rest) = id.split_once('/')?;
        if rest.is_empty() {
            return None;
        }
        Some(match prefix {
            "claude-code" => Self::ClaudeCode { model: rest.into() },
            "codex" => Self::Codex { model: rest.into() },
            "nanogpt" => Self::NanoGpt { model: rest.into() },
            "cf-gateway" => {
                let (provider, model) = rest.split_once('/')?;
                if provider.is_empty() || model.is_empty() {
                    return None;
                }
                Self::CfGateway {
                    provider: provider.into(),
                    model: model.into(),
                }
            }
            _ => return None,
        })
    }

    pub fn id(&self) -> String {
        match self {
            Self::ClaudeCode { model } => format!("claude-code/{model}"),
            Self::Codex { model } => format!("codex/{model}"),
            Self::CfGateway { provider, model } => format!("cf-gateway/{provider}/{model}"),
            Self::NanoGpt { model } => format!("nanogpt/{model}"),
        }
    }

    pub fn provider(&self) -> &'static str {
        match self {
            Self::ClaudeCode { .. } => "claude-code",
            Self::Codex { .. } => "codex",
            Self::CfGateway { .. } => "cf-gateway",
            Self::NanoGpt { .. } => "nanogpt",
        }
    }

    /// CLI providers run tools themselves and accept MCP servers.
    pub fn is_cli(&self) -> bool {
        matches!(self, Self::ClaudeCode { .. } | Self::Codex { .. })
    }
}

/// Provider-neutral generation events.
#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum GenEvent {
    Text {
        text: String,
    },
    ToolUse {
        id: String,
        name: String,
        input: Value,
    },
    ToolResult {
        tool_use_id: String,
        is_error: bool,
        content: String,
    },
    Usage {
        prompt_tokens: u64,
        completion_tokens: u64,
    },
    Error {
        message: String,
    },
    Done {
        finish_reason: String,
    },
}

impl GenEvent {
    pub fn is_terminal(&self) -> bool {
        matches!(self, Self::Error { .. } | Self::Done { .. })
    }
}

/// Max bytes of tool output carried in events.
pub const TOOL_CONTENT_MAX: usize = 4096;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
pub struct Capabilities {
    pub vision: bool,
    pub tools: bool,
}

/// One `GET /v1/models` entry. `display_name`, `capabilities` and
/// `context_length` are what the browser reads (ko_agentd_models.cc).
#[derive(Debug, Clone, Serialize)]
pub struct ModelInfo {
    pub id: String,
    pub object: &'static str,
    pub created: i64,
    pub owned_by: &'static str,
    pub display_name: String,
    pub capabilities: Capabilities,
    pub context_length: u32,
}

/// "<model> (<provider>)", cut to the browser's display-name limit.
fn default_display_name(r: &ModelRef) -> String {
    let (label, model) = match r {
        ModelRef::ClaudeCode { model } => ("Claude Code", model.clone()),
        ModelRef::Codex { model } => ("Codex", model.clone()),
        ModelRef::CfGateway { provider, model } => ("AI Gateway", format!("{provider}/{model}")),
        ModelRef::NanoGpt { model } => ("NanoGPT", model.clone()),
    };
    let suffix = format!(" ({label})");
    let room = MAX_DISPLAY_NAME - suffix.len();
    format!("{}{suffix}", crate::util::truncate(&model, room))
}

fn model_info(r: ModelRef, over: Option<&ModelInfoConfig>) -> ModelInfo {
    let o = over.cloned().unwrap_or_default();
    // CLI providers run their own tools and take text only. Config already
    // rejects vision/tools = true for them.
    let http = !r.is_cli();
    ModelInfo {
        display_name: o
            .display_name
            .map(|n| n.trim().to_owned())
            .unwrap_or_else(|| default_display_name(&r)),
        capabilities: Capabilities {
            vision: http && o.vision.unwrap_or(false),
            tools: http && o.tools.unwrap_or(true),
        },
        context_length: o.context_length.unwrap_or(DEFAULT_CONTEXT_LENGTH),
        id: r.id(),
        object: "model",
        created: 0,
        owned_by: r.provider(),
    }
}

/// Configured, enabled models, by provider then `models` order. The
/// browser's default is the first one.
pub fn list_models(cfg: &Config) -> Vec<ModelInfo> {
    let mut out = Vec::new();
    let p = &cfg.providers;
    if let Some(c) = p.claude_code.as_ref().filter(|c| c.enabled) {
        for m in &c.models {
            let r = ModelRef::ClaudeCode { model: m.clone() };
            out.push(model_info(r, c.model_info.get(m)));
        }
    }
    if let Some(c) = p.codex.as_ref().filter(|c| c.enabled) {
        for m in &c.models {
            let r = ModelRef::Codex { model: m.clone() };
            out.push(model_info(r, c.model_info.get(m)));
        }
    }
    if let Some(c) = p.cf_gateway.as_ref().filter(|c| c.enabled) {
        for m in &c.models {
            if let Some((provider, model)) = m.split_once('/') {
                let r = ModelRef::CfGateway {
                    provider: provider.into(),
                    model: model.into(),
                };
                out.push(model_info(r, c.model_info.get(m)));
            }
        }
    }
    if let Some(c) = p.nanogpt.as_ref().filter(|c| c.enabled) {
        for m in &c.models {
            let r = ModelRef::NanoGpt { model: m.clone() };
            out.push(model_info(r, c.model_info.get(m)));
        }
    }
    out
}

/// "claude-code/default" and "codex/default" mean the first model configured
/// for that provider. The browser sends the former before it has listed
/// models. A configured model literally named "default" wins. Not listed.
pub const DEFAULT_ALIAS: &str = "default";

fn is_default_alias(r: &ModelRef) -> bool {
    matches!(r, ModelRef::ClaudeCode { model } | ModelRef::Codex { model } if model == DEFAULT_ALIAS)
}

/// First model of an enabled CLI provider, for a "<provider>/default" alias.
fn alias_target(cfg: &Config, r: &ModelRef) -> Option<ModelRef> {
    let p = &cfg.providers;
    match r {
        ModelRef::ClaudeCode { .. } => {
            let c = p.claude_code.as_ref().filter(|c| c.enabled)?;
            let model = c.models.first()?.clone();
            Some(ModelRef::ClaudeCode { model })
        }
        ModelRef::Codex { .. } => {
            let c = p.codex.as_ref().filter(|c| c.enabled)?;
            let model = c.models.first()?.clone();
            Some(ModelRef::Codex { model })
        }
        _ => None,
    }
}

/// Resolves an id (or a "<cli-provider>/default" alias) to a configured,
/// enabled model.
pub fn resolve(cfg: &Arc<Config>, id: &str) -> Result<ModelRef, ApiError> {
    let not_found =
        |m: String| ApiError::new(axum::http::StatusCode::NOT_FOUND, "model_not_found", m);
    let r = ModelRef::parse(id).ok_or_else(|| {
        not_found(format!(
            "unknown model {:?}",
            crate::util::truncate(id, 200)
        ))
    })?;
    if list_models(cfg).iter().any(|m| m.id == r.id()) {
        return Ok(r);
    }
    if is_default_alias(&r) {
        let p = r.provider();
        return alias_target(cfg, &r).ok_or_else(|| {
            not_found(format!(
                "model {:?} needs the {p} provider, which is not configured or is \
                 disabled ([providers.{p}] in agentd.toml)",
                r.id()
            ))
        });
    }
    Err(not_found(format!("model {:?} is not configured", r.id())))
}

/// A running generation. Keeps the private run dir alive until dropped.
pub struct Generation {
    pub events: tokio::sync::mpsc::Receiver<GenEvent>,
    pub _tmp: Option<crate::paths::PrivateTempDir>,
}

pub struct GenParams<'a> {
    pub model: &'a ModelRef,
    pub prompt: zeroize::Zeroizing<String>,
    pub mcp: &'a [(String, crate::config::McpServerDef)],
    /// Working dir for CLI providers (default: a fresh private dir).
    pub cwd: Option<std::path::PathBuf>,
    /// Agent runs get the configured permission mode / sandbox.
    pub agent_run: bool,
    pub timeout: Option<std::time::Duration>,
    pub cancel: tokio_util::sync::CancellationToken,
}

fn internal(e: anyhow::Error) -> ApiError {
    let msg = crate::telemetry::scrub(&format!("{e:#}"));
    if msg.contains("not found on PATH") {
        ApiError::unavailable(msg)
    } else {
        ApiError::provider(msg)
    }
}

/// Starts a generation on any provider.
pub async fn generate(
    core: &crate::server::Core,
    p: GenParams<'_>,
) -> Result<Generation, ApiError> {
    use std::time::Duration;
    let cfg = &core.cfg;
    if !p.model.is_cli() {
        if !p.mcp.is_empty() {
            return Err(ApiError::bad_request(
                "MCP servers require a CLI provider (claude-code or codex)",
            ));
        }
        let up = http::upstream(cfg, &core.secrets, p.model).await?;
        let total = p.timeout.unwrap_or(up.timeout);
        let events = http::generate(core.http.clone(), up, &p.prompt, total, p.cancel).await?;
        return Ok(Generation { events, _tmp: None });
    }

    let tmp = crate::paths::PrivateTempDir::new(&core.paths, "gen").map_err(ApiError::internal)?;
    let tmpdir = tmp.path().join("tmp");
    std::fs::create_dir(&tmpdir).map_err(|e| ApiError::internal(e.into()))?;
    let cwd = match p.cwd {
        Some(c) => c,
        None => {
            let w = tmp.path().join("work");
            std::fs::create_dir(&w).map_err(|e| ApiError::internal(e.into()))?;
            w
        }
    };
    let secrets = core.secrets.clone();
    let mcp = p.mcp.to_vec();
    let spec = match p.model {
        ModelRef::ClaudeCode { model } => {
            let c = cfg
                .providers
                .claude_code
                .as_ref()
                .ok_or_else(|| ApiError::unavailable("claude-code is not configured"))?;
            let mcp_path = if mcp.is_empty() {
                None
            } else {
                let json =
                    tokio::task::spawn_blocking(move || crate::mcp::claude_config(&mcp, &secrets))
                        .await
                        .map_err(|e| ApiError::internal(e.into()))?
                        .map_err(|e| {
                            ApiError::unavailable(crate::telemetry::scrub(&format!("{e:#}")))
                        })?;
                Some(
                    crate::mcp::write_claude_config(tmp.path(), &json)
                        .map_err(ApiError::internal)?,
                )
            };
            cli::CliSpec {
                program: c.binary.clone(),
                args: claude::args(c, model, mcp_path.as_deref(), p.agent_run),
                env: vec![],
                pass_env: c.pass_env.clone(),
                cwd,
                tmpdir,
                stdin: p.prompt,
                timeout: p.timeout.unwrap_or(Duration::from_secs(c.timeout_secs)),
                cancel: p.cancel,
                delete_after_start: mcp_path,
            }
        }
        ModelRef::Codex { model } => {
            let c = cfg
                .providers
                .codex
                .as_ref()
                .ok_or_else(|| ApiError::unavailable("codex is not configured"))?;
            let o =
                tokio::task::spawn_blocking(move || crate::mcp::codex_overrides(&mcp, &secrets))
                    .await
                    .map_err(|e| ApiError::internal(e.into()))?
                    .map_err(|e| {
                        ApiError::unavailable(crate::telemetry::scrub(&format!("{e:#}")))
                    })?;
            cli::CliSpec {
                program: c.binary.clone(),
                args: codex::args(c, model, &o.args, p.agent_run),
                env: o.env,
                pass_env: c.pass_env.clone(),
                cwd,
                tmpdir,
                stdin: p.prompt,
                timeout: p.timeout.unwrap_or(Duration::from_secs(c.timeout_secs)),
                cancel: p.cancel,
                delete_after_start: None,
            }
        }
        _ => unreachable!("is_cli checked"),
    };
    let mapper: Box<dyn cli::LineMapper> = match p.model {
        ModelRef::ClaudeCode { .. } => Box::<claude::ClaudeMapper>::default(),
        _ => Box::<codex::CodexMapper>::default(),
    };
    let events = cli::spawn(spec, mapper).map_err(internal)?;
    Ok(Generation {
        events,
        _tmp: Some(tmp),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn model_ids() {
        for id in [
            "claude-code/sonnet",
            "codex/gpt-5-codex",
            "cf-gateway/openai/gpt-4o-mini",
            "cf-gateway/workers-ai/@cf/meta/llama-3.1-8b-instruct",
            "nanogpt/chatgpt-4o-latest",
        ] {
            assert_eq!(ModelRef::parse(id).unwrap().id(), id);
        }
        assert!(ModelRef::parse("cf-gateway/openai").is_none());
        assert!(ModelRef::parse("other/x").is_none());
        assert!(ModelRef::parse("claude-code/").is_none());
        assert!(ModelRef::parse("sonnet").is_none());
    }

    #[test]
    fn resolve_requires_configuration() {
        let cfg =
            Arc::new(Config::from_toml("[providers.claude-code]\nmodels=[\"sonnet\"]").unwrap());
        assert!(resolve(&cfg, "claude-code/sonnet").is_ok());
        assert!(resolve(&cfg, "claude-code/opus").is_err());
        assert!(resolve(&cfg, "nanogpt/x").is_err());
        assert_eq!(list_models(&cfg).len(), 1);
    }

    const ALL: &str = r#"
[providers.claude-code]
models = ["opus", "sonnet"]
[providers.codex]
models = ["gpt-5-codex"]
[providers.nanogpt]
models = ["chatgpt-4o-latest"]
"#;

    #[test]
    fn default_alias_resolves_to_first_cli_model() {
        let cfg = Arc::new(Config::from_toml(ALL).unwrap());
        let r = resolve(&cfg, "claude-code/default").unwrap();
        assert_eq!(r.id(), "claude-code/opus");
        assert_eq!(
            resolve(&cfg, "codex/default").unwrap().id(),
            "codex/gpt-5-codex"
        );
        // HTTP providers have no alias; "default" could be an upstream id.
        assert!(resolve(&cfg, "nanogpt/default").is_err());
        // Never listed.
        assert!(
            list_models(&cfg)
                .iter()
                .all(|m| !m.id.ends_with("/default"))
        );

        // A configured model named "default" is used as is.
        let lit = Arc::new(
            Config::from_toml("[providers.claude-code]\nmodels=[\"sonnet\", \"default\"]").unwrap(),
        );
        assert_eq!(
            resolve(&lit, "claude-code/default").unwrap().id(),
            "claude-code/default"
        );
    }

    #[test]
    fn default_alias_names_the_missing_provider() {
        for toml in [
            "[providers.nanogpt]\nmodels=[\"x\"]",
            "[providers.claude-code]\nenabled=false\nmodels=[\"sonnet\"]",
        ] {
            let cfg = Arc::new(Config::from_toml(toml).unwrap());
            let e = resolve(&cfg, "claude-code/default").unwrap_err();
            assert_eq!(e.status, axum::http::StatusCode::NOT_FOUND);
            assert_eq!(e.kind, "model_not_found");
            assert!(e.message.contains("claude-code provider"), "{}", e.message);
        }
    }

    #[test]
    fn model_info_defaults() {
        let cfg = Config::from_toml(ALL).unwrap();
        let m = list_models(&cfg);
        let ids: Vec<&str> = m.iter().map(|m| m.id.as_str()).collect();
        assert_eq!(
            ids,
            [
                "claude-code/opus",
                "claude-code/sonnet",
                "codex/gpt-5-codex",
                "nanogpt/chatgpt-4o-latest"
            ]
        );
        for cli in &m[..3] {
            assert_eq!(
                cli.capabilities,
                Capabilities {
                    vision: false,
                    tools: false
                }
            );
            assert_eq!(cli.context_length, DEFAULT_CONTEXT_LENGTH);
        }
        assert_eq!(m[0].display_name, "opus (Claude Code)");
        assert_eq!(m[2].display_name, "gpt-5-codex (Codex)");
        assert_eq!(m[3].display_name, "chatgpt-4o-latest (NanoGPT)");
        assert_eq!(
            m[3].capabilities,
            Capabilities {
                vision: false,
                tools: true
            }
        );
        assert_eq!(m[3].context_length, 128_000);

        let v = serde_json::to_value(&m[3]).unwrap();
        assert_eq!(
            v,
            serde_json::json!({
                "id": "nanogpt/chatgpt-4o-latest", "object": "model", "created": 0,
                "owned_by": "nanogpt", "display_name": "chatgpt-4o-latest (NanoGPT)",
                "capabilities": {"vision": false, "tools": true}, "context_length": 128000
            })
        );

        let long = "m".repeat(128);
        let cfg = Config::from_toml(&format!("[providers.nanogpt]\nmodels=[\"{long}\"]")).unwrap();
        let name = &list_models(&cfg)[0].display_name;
        assert_eq!(name.len(), MAX_DISPLAY_NAME);
        assert!(name.ends_with(" (NanoGPT)"));
    }

    #[test]
    fn model_info_overrides() {
        let cfg = Config::from_toml(
            r#"
[providers.claude-code]
models = ["sonnet"]
[providers.claude-code.model_info.sonnet]
display_name = "  Claude Sonnet  "
context_length = 200000
[providers.cf-gateway]
account_id = "0123456789abcdef0123456789abcdef"
gateway = "g"
models = ["openai/gpt-4o-mini", "workers-ai/@cf/meta/llama-3.1-8b-instruct"]
[providers.cf-gateway.model_info."openai/gpt-4o-mini"]
vision = true
[providers.cf-gateway.model_info."workers-ai/@cf/meta/llama-3.1-8b-instruct"]
tools = false
context_length = 8000
"#,
        )
        .unwrap();
        let m = list_models(&cfg);
        assert_eq!(m[0].display_name, "Claude Sonnet");
        assert_eq!(m[0].context_length, 200_000);
        assert!(!m[0].capabilities.tools);
        assert_eq!(m[1].id, "cf-gateway/openai/gpt-4o-mini");
        assert_eq!(m[1].display_name, "openai/gpt-4o-mini (AI Gateway)");
        assert_eq!(
            m[1].capabilities,
            Capabilities {
                vision: true,
                tools: true
            }
        );
        assert_eq!(
            m[2].capabilities,
            Capabilities {
                vision: false,
                tools: false
            }
        );
        assert_eq!(m[2].context_length, 8000);
    }
}
