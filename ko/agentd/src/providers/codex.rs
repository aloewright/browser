// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

//! Codex CLI: `codex exec --json -` (JSONL events, prompt on stdin).

use serde_json::{Value, json};

use super::cli::LineMapper;
use super::{GenEvent, TOOL_CONTENT_MAX};
use crate::config::CodexConfig;
use crate::util::truncate;

pub fn args(
    cfg: &CodexConfig,
    model: &str,
    mcp_overrides: &[String],
    agent_run: bool,
) -> Vec<String> {
    let sandbox = if agent_run {
        cfg.sandbox.as_str()
    } else {
        "read-only"
    };
    let mut a: Vec<String> = [
        "exec",
        "--json",
        "--skip-git-repo-check",
        "--sandbox",
        sandbox,
        "--model",
        model,
    ]
    .iter()
    .map(|s| s.to_string())
    .collect();
    if cfg.ephemeral {
        a.push("--ephemeral".into());
    }
    a.extend(mcp_overrides.iter().cloned());
    a.push("-".into());
    a
}

#[derive(Default)]
pub struct CodexMapper {
    text_emitted: bool,
}

impl CodexMapper {
    fn text(&mut self, t: &str) -> Option<GenEvent> {
        if t.is_empty() {
            return None;
        }
        let text = if self.text_emitted {
            format!("\n\n{t}")
        } else {
            t.to_owned()
        };
        self.text_emitted = true;
        Some(GenEvent::Text { text })
    }

    fn item_started(item: &Value) -> Option<GenEvent> {
        let id = item["id"].as_str().unwrap_or("").to_owned();
        let (name, input) = match item["type"].as_str()? {
            "command_execution" => (
                "command_execution".to_owned(),
                json!({"command": item["command"]}),
            ),
            "mcp_tool_call" => (
                format!(
                    "mcp:{}/{}",
                    item["server"].as_str().unwrap_or(""),
                    item["tool"].as_str().unwrap_or("")
                ),
                item.get("arguments").cloned().unwrap_or(json!({})),
            ),
            "web_search" => ("web_search".to_owned(), json!({"query": item["query"]})),
            _ => return None,
        };
        Some(GenEvent::ToolUse { id, name, input })
    }

    fn item_completed(&mut self, item: &Value) -> Vec<GenEvent> {
        let id = item["id"].as_str().unwrap_or("").to_owned();
        match item["type"].as_str().unwrap_or("") {
            "agent_message" => self
                .text(item["text"].as_str().unwrap_or(""))
                .into_iter()
                .collect(),
            "command_execution" => {
                let output = item["aggregated_output"].as_str().unwrap_or("");
                let failed = item["status"].as_str() == Some("failed")
                    || item["exit_code"].as_i64().is_some_and(|c| c != 0);
                vec![GenEvent::ToolResult {
                    tool_use_id: id,
                    is_error: failed,
                    content: truncate(output, TOOL_CONTENT_MAX).to_owned(),
                }]
            }
            "mcp_tool_call" => {
                let failed = item["status"].as_str() == Some("failed");
                let content = item
                    .get("result")
                    .or_else(|| item.get("error"))
                    .map(Value::to_string)
                    .unwrap_or_default();
                vec![GenEvent::ToolResult {
                    tool_use_id: id,
                    is_error: failed,
                    content: truncate(&content, TOOL_CONTENT_MAX).to_owned(),
                }]
            }
            "file_change" => vec![GenEvent::ToolUse {
                id,
                name: "file_change".into(),
                input: json!({"changes": item["changes"], "status": item["status"]}),
            }],
            _ => vec![],
        }
    }
}

impl LineMapper for CodexMapper {
    fn map(&mut self, v: &Value) -> Vec<GenEvent> {
        match v.get("type").and_then(Value::as_str) {
            Some("item.started") => Self::item_started(&v["item"]).into_iter().collect(),
            Some("item.completed") => self.item_completed(&v["item"]),
            Some("turn.completed") => {
                let usage = &v["usage"];
                vec![
                    GenEvent::Usage {
                        prompt_tokens: usage["input_tokens"].as_u64().unwrap_or(0),
                        completion_tokens: usage["output_tokens"].as_u64().unwrap_or(0),
                    },
                    GenEvent::Done {
                        finish_reason: "stop".into(),
                    },
                ]
            }
            Some("turn.failed") => vec![GenEvent::Error {
                message: format!(
                    "codex: {}",
                    truncate(
                        v["error"]["message"].as_str().unwrap_or("turn failed"),
                        1024
                    )
                ),
            }],
            Some("error") => vec![GenEvent::Error {
                message: format!(
                    "codex: {}",
                    truncate(v["message"].as_str().unwrap_or("error"), 1024)
                ),
            }],
            // Legacy `{"id":..,"msg":{...}}` event stream (older codex releases).
            None if v.get("msg").is_some() => {
                let msg = &v["msg"];
                match msg["type"].as_str() {
                    Some("agent_message") => self
                        .text(msg["message"].as_str().unwrap_or(""))
                        .into_iter()
                        .collect(),
                    Some("task_complete") => vec![GenEvent::Done {
                        finish_reason: "stop".into(),
                    }],
                    Some("error") => vec![GenEvent::Error {
                        message: format!(
                            "codex: {}",
                            truncate(msg["message"].as_str().unwrap_or("error"), 1024)
                        ),
                    }],
                    _ => vec![],
                }
            }
            _ => vec![],
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const FIXTURE: &str = include_str!("../../tests/fixtures/codex_stream.jsonl");

    fn map_all(s: &str) -> Vec<GenEvent> {
        let mut m = CodexMapper::default();
        s.lines()
            .filter_map(|l| serde_json::from_str::<Value>(l).ok())
            .flat_map(|v| m.map(&v))
            .collect()
    }

    #[test]
    fn maps_recorded_stream() {
        let evs = map_all(FIXTURE);
        assert!(matches!(&evs[0], GenEvent::ToolUse { id, name, input }
            if id == "item_1" && name == "command_execution" && input["command"] == "bash -lc ls"));
        assert!(
            matches!(&evs[1], GenEvent::ToolResult { tool_use_id, is_error: false, content }
            if tool_use_id == "item_1" && content.contains("README.md"))
        );
        assert_eq!(
            evs[2],
            GenEvent::Text {
                text: "Repo contains docs, sdk, and examples directories.".into()
            }
        );
        assert_eq!(
            evs[3],
            GenEvent::Usage {
                prompt_tokens: 24763,
                completion_tokens: 122
            }
        );
        assert!(matches!(&evs[4], GenEvent::Done { .. }));
        assert_eq!(evs.len(), 5);
    }

    #[test]
    fn maps_failures_and_legacy() {
        let evs = map_all(r#"{"type":"turn.failed","error":{"message":"usage limit reached"}}"#);
        assert_eq!(
            evs,
            vec![GenEvent::Error {
                message: "codex: usage limit reached".into()
            }]
        );
        let legacy = map_all(
            "{\"id\":\"0\",\"msg\":{\"type\":\"agent_message\",\"message\":\"hi\"}}\n\
             {\"id\":\"0\",\"msg\":{\"type\":\"task_complete\"}}",
        );
        assert_eq!(legacy.len(), 2);
    }

    #[test]
    fn argv() {
        let cfg = CodexConfig {
            sandbox: "workspace-write".into(),
            ..Default::default()
        };
        let a = args(&cfg, "gpt-5-codex", &["-c".into(), "x=1".into()], true);
        assert_eq!(a[0], "exec");
        assert!(a.windows(2).any(|w| w == ["--sandbox", "workspace-write"]));
        assert!(a.contains(&"--ephemeral".to_string()));
        assert_eq!(a.last().unwrap(), "-");
        let chat = args(&cfg, "gpt-5-codex", &[], false);
        assert!(chat.windows(2).any(|w| w == ["--sandbox", "read-only"]));
    }
}
