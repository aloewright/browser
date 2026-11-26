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

use crate::config::Config;
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

#[derive(Debug, Clone, Serialize)]
pub struct ModelInfo {
    pub id: String,
    pub object: &'static str,
    pub created: i64,
    pub owned_by: &'static str,
}

/// Configured, enabled models.
pub fn list_models(cfg: &Config) -> Vec<ModelInfo> {
    let mut out = Vec::new();
    let mut push = |r: ModelRef| {
        out.push(ModelInfo {
            id: r.id(),
            object: "model",
            created: 0,
            owned_by: r.provider(),
        })
    };
    let p = &cfg.providers;
    if let Some(c) = p.claude_code.as_ref().filter(|c| c.enabled) {
        for m in &c.models {
            push(ModelRef::ClaudeCode { model: m.clone() });
        }
    }
    if let Some(c) = p.codex.as_ref().filter(|c| c.enabled) {
        for m in &c.models {
            push(ModelRef::Codex { model: m.clone() });
        }
    }
    if let Some(c) = p.cf_gateway.as_ref().filter(|c| c.enabled) {
        for m in &c.models {
            if let Some((provider, model)) = m.split_once('/') {
                push(ModelRef::CfGateway {
                    provider: provider.into(),
                    model: model.into(),
                });
            }
        }
    }
    if let Some(c) = p.nanogpt.as_ref().filter(|c| c.enabled) {
        for m in &c.models {
            push(ModelRef::NanoGpt { model: m.clone() });
        }
    }
    out
}

/// Resolves an id to a configured, enabled model.
pub fn resolve(cfg: &Arc<Config>, id: &str) -> Result<ModelRef, ApiError> {
    let r = ModelRef::parse(id).ok_or_else(|| {
        ApiError::new(
            axum::http::StatusCode::NOT_FOUND,
            "model_not_found",
            format!("unknown model {:?}", crate::util::truncate(id, 200)),
        )
    })?;
    if list_models(cfg).iter().any(|m| m.id == r.id()) {
        Ok(r)
    } else {
        Err(ApiError::new(
            axum::http::StatusCode::NOT_FOUND,
            "model_not_found",
            format!("model {:?} is not configured", r.id()),
        ))
    }
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
        let events = http::generate(core.http.clone(), up, &p.prompt, p.cancel).await?;
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
}
