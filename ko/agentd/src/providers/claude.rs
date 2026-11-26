// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

//! Claude Code CLI: `claude -p --output-format stream-json --verbose`.

use std::path::Path;

use serde_json::{Value, json};

use super::cli::LineMapper;
use super::{GenEvent, TOOL_CONTENT_MAX};
use crate::config::ClaudeCodeConfig;
use crate::util::truncate;

/// Arguments for one headless invocation; the prompt is sent on stdin.
pub fn args(
    cfg: &ClaudeCodeConfig,
    model: &str,
    mcp_config: Option<&Path>,
    agent_run: bool,
) -> Vec<String> {
    let mut a: Vec<String> = [
        "-p",
        "--output-format",
        "stream-json",
        "--verbose",
        "--model",
        model,
    ]
    .iter()
    .map(|s| s.to_string())
    .collect();
    if let Some(p) = mcp_config {
        a.push("--mcp-config".into());
        a.push(p.display().to_string());
        a.push("--strict-mcp-config".into());
    }
    if agent_run {
        if let Some(m) = &cfg.permission_mode {
            a.push("--permission-mode".into());
            a.push(m.clone());
        }
        if !cfg.allowed_tools.is_empty() {
            a.push("--allowedTools".into());
            a.push(cfg.allowed_tools.join(","));
        }
    }
    a
}

fn tool_result_text(content: &Value) -> String {
    match content {
        Value::String(s) => s.clone(),
        Value::Array(items) => items
            .iter()
            .filter_map(|i| i.get("text").and_then(Value::as_str))
            .collect::<Vec<_>>()
            .join("\n"),
        Value::Null => String::new(),
        other => other.to_string(),
    }
}

fn u(v: &Value, k: &str) -> u64 {
    v.get(k).and_then(Value::as_u64).unwrap_or(0)
}

#[derive(Default)]
pub struct ClaudeMapper {
    text_emitted: bool,
}

impl LineMapper for ClaudeMapper {
    fn map(&mut self, v: &Value) -> Vec<GenEvent> {
        let mut out = Vec::new();
        match v.get("type").and_then(Value::as_str) {
            Some("assistant") => {
                let blocks = v["message"]["content"]
                    .as_array()
                    .cloned()
                    .unwrap_or_default();
                for b in blocks {
                    match b.get("type").and_then(Value::as_str) {
                        Some("text") => {
                            let t = b.get("text").and_then(Value::as_str).unwrap_or("");
                            if t.is_empty() {
                                continue;
                            }
                            let text = if self.text_emitted {
                                format!("\n\n{t}")
                            } else {
                                t.to_owned()
                            };
                            self.text_emitted = true;
                            out.push(GenEvent::Text { text });
                        }
                        Some("tool_use") => out.push(GenEvent::ToolUse {
                            id: b["id"].as_str().unwrap_or("").to_owned(),
                            name: b["name"].as_str().unwrap_or("").to_owned(),
                            input: b.get("input").cloned().unwrap_or(json!({})),
                        }),
                        _ => {}
                    }
                }
            }
            Some("user") => {
                let blocks = v["message"]["content"]
                    .as_array()
                    .cloned()
                    .unwrap_or_default();
                for b in blocks {
                    if b.get("type").and_then(Value::as_str) == Some("tool_result") {
                        let text = tool_result_text(b.get("content").unwrap_or(&Value::Null));
                        out.push(GenEvent::ToolResult {
                            tool_use_id: b["tool_use_id"].as_str().unwrap_or("").to_owned(),
                            is_error: b["is_error"].as_bool().unwrap_or(false),
                            content: truncate(&text, TOOL_CONTENT_MAX).to_owned(),
                        });
                    }
                }
            }
            Some("result") => {
                let is_error = v["is_error"].as_bool().unwrap_or(false);
                let subtype = v["subtype"].as_str().unwrap_or("");
                if is_error || subtype != "success" {
                    let msg = v["result"]
                        .as_str()
                        .filter(|s| !s.is_empty())
                        .unwrap_or(if subtype.is_empty() { "error" } else { subtype });
                    out.push(GenEvent::Error {
                        message: format!("claude: {}", truncate(msg, 1024)),
                    });
                } else {
                    let usage = &v["usage"];
                    out.push(GenEvent::Usage {
                        prompt_tokens: u(usage, "input_tokens")
                            + u(usage, "cache_read_input_tokens")
                            + u(usage, "cache_creation_input_tokens"),
                        completion_tokens: u(usage, "output_tokens"),
                    });
                    out.push(GenEvent::Done {
                        finish_reason: "stop".into(),
                    });
                }
            }
            _ => {}
        }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    pub const FIXTURE: &str = include_str!("../../tests/fixtures/claude_stream.jsonl");

    fn map_all(s: &str) -> Vec<GenEvent> {
        let mut m = ClaudeMapper::default();
        s.lines()
            .filter_map(|l| serde_json::from_str::<Value>(l).ok())
            .flat_map(|v| m.map(&v))
            .collect()
    }

    #[test]
    fn maps_recorded_stream() {
        let evs = map_all(FIXTURE);
        assert_eq!(
            evs[0],
            GenEvent::Text {
                text: "I'll list the files.".into()
            }
        );
        assert!(matches!(&evs[1], GenEvent::ToolUse { name, input, .. }
            if name == "Bash" && input["command"] == "ls"));
        assert!(
            matches!(&evs[2], GenEvent::ToolResult { tool_use_id, is_error: false, content }
            if tool_use_id == "toolu_01" && content.contains("README.md"))
        );
        assert_eq!(
            evs[3],
            GenEvent::Text {
                text: "\n\nThe directory contains README.md and src.".into()
            }
        );
        assert_eq!(
            evs[4],
            GenEvent::Usage {
                prompt_tokens: 1250,
                completion_tokens: 42
            }
        );
        assert_eq!(
            evs[5],
            GenEvent::Done {
                finish_reason: "stop".into()
            }
        );
        assert_eq!(evs.len(), 6);
    }

    #[test]
    fn maps_errors() {
        let evs =
            map_all(r#"{"type":"result","subtype":"error_max_turns","is_error":true,"result":""}"#);
        assert_eq!(
            evs,
            vec![GenEvent::Error {
                message: "claude: error_max_turns".into()
            }]
        );
    }

    #[test]
    fn argv_has_no_prompt() {
        let cfg = ClaudeCodeConfig {
            permission_mode: Some("acceptEdits".into()),
            allowed_tools: vec!["Read".into(), "Bash(git:*)".into()],
            ..Default::default()
        };
        let a = args(&cfg, "sonnet", Some(Path::new("/tmp/x/mcp.json")), true);
        assert_eq!(
            &a[..6],
            [
                "-p",
                "--output-format",
                "stream-json",
                "--verbose",
                "--model",
                "sonnet"
            ]
        );
        assert!(
            a.windows(2)
                .any(|w| w == ["--mcp-config", "/tmp/x/mcp.json"])
        );
        assert!(a.contains(&"--strict-mcp-config".to_string()));
        assert!(
            a.windows(2)
                .any(|w| w == ["--permission-mode", "acceptEdits"])
        );
        assert!(
            a.windows(2)
                .any(|w| w == ["--allowedTools", "Read,Bash(git:*)"])
        );
        let chat = args(&cfg, "sonnet", None, false);
        assert_eq!(chat.len(), 6);
    }
}
