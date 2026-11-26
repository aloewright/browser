// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

//! Maps provider events to OpenAI chat.completion(.chunk) objects. Tool
//! activity of CLI providers (which run tools themselves) is reported in the
//! non-standard "ko_tool_activity" field so OpenAI clients do not try to
//! execute it.

use serde_json::{Value, json};

use super::{GenEvent, TOOL_CONTENT_MAX};
use crate::error::ApiError;
use crate::util::truncate;

fn activity(ev: &GenEvent) -> Option<Value> {
    match ev {
        GenEvent::ToolUse { id, name, input } => {
            Some(json!({"type": "tool_use", "id": id, "name": name, "input": input}))
        }
        GenEvent::ToolResult {
            tool_use_id,
            is_error,
            content,
        } => Some(json!({
            "type": "tool_result",
            "tool_use_id": tool_use_id,
            "is_error": is_error,
            "content": truncate(content, TOOL_CONTENT_MAX),
        })),
        _ => None,
    }
}

fn usage_json(prompt: u64, completion: u64) -> Value {
    json!({
        "prompt_tokens": prompt,
        "completion_tokens": completion,
        "total_tokens": prompt + completion,
    })
}

pub struct ChunkBuilder {
    id: String,
    created: i64,
    model: String,
    sent_role: bool,
    usage: Option<(u64, u64)>,
}

impl ChunkBuilder {
    pub fn new(model: &str) -> Self {
        Self {
            id: format!("chatcmpl-{}", uuid::Uuid::new_v4().simple()),
            created: crate::util::now_secs(),
            model: model.to_owned(),
            sent_role: false,
            usage: None,
        }
    }

    fn chunk(&self, delta: Value, finish: Option<&str>) -> Value {
        json!({
            "id": self.id,
            "object": "chat.completion.chunk",
            "created": self.created,
            "model": self.model,
            "choices": [{"index": 0, "delta": delta, "finish_reason": finish}],
        })
    }

    /// One chunk per event, or None (usage is held for the final chunk).
    pub fn event(&mut self, ev: &GenEvent) -> Option<Value> {
        match ev {
            GenEvent::Text { text } => {
                let mut delta = json!({ "content": text });
                if !self.sent_role {
                    delta["role"] = json!("assistant");
                    self.sent_role = true;
                }
                Some(self.chunk(delta, None))
            }
            GenEvent::ToolUse { .. } | GenEvent::ToolResult { .. } => {
                let mut c = self.chunk(json!({}), None);
                c["ko_tool_activity"] = activity(ev).unwrap_or(Value::Null);
                Some(c)
            }
            GenEvent::Usage {
                prompt_tokens,
                completion_tokens,
            } => {
                self.usage = Some((*prompt_tokens, *completion_tokens));
                None
            }
            GenEvent::Done { finish_reason } => {
                let mut c = self.chunk(json!({}), Some(finish_reason));
                if let Some((p, o)) = self.usage {
                    c["usage"] = usage_json(p, o);
                }
                Some(c)
            }
            GenEvent::Error { message } => Some(error_chunk(message)),
        }
    }
}

pub fn error_chunk(message: &str) -> Value {
    json!({ "error": { "message": message, "type": "provider_error" } })
}

/// Collapses a finished event list into a chat.completion response.
pub fn aggregate(model: &str, events: &[GenEvent]) -> Result<Value, ApiError> {
    let mut text = String::new();
    let mut tools = Vec::new();
    let mut usage = None;
    let mut finish = None;
    for ev in events {
        match ev {
            GenEvent::Text { text: t } => text.push_str(t),
            GenEvent::ToolUse { .. } | GenEvent::ToolResult { .. } => {
                tools.extend(activity(ev));
            }
            GenEvent::Usage {
                prompt_tokens,
                completion_tokens,
            } => usage = Some(usage_json(*prompt_tokens, *completion_tokens)),
            GenEvent::Error { message } => return Err(ApiError::provider(message.clone())),
            GenEvent::Done { finish_reason } => finish = Some(finish_reason.clone()),
        }
    }
    let finish = finish.ok_or_else(|| ApiError::provider("provider ended without a result"))?;
    let mut out = json!({
        "id": format!("chatcmpl-{}", uuid::Uuid::new_v4().simple()),
        "object": "chat.completion",
        "created": crate::util::now_secs(),
        "model": model,
        "choices": [{
            "index": 0,
            "message": {"role": "assistant", "content": text},
            "finish_reason": finish,
        }],
    });
    if let Some(u) = usage {
        out["usage"] = u;
    }
    if !tools.is_empty() {
        out["ko_tool_activity"] = Value::Array(tools);
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn chunks_and_aggregate() {
        let evs = vec![
            GenEvent::Text { text: "Hel".into() },
            GenEvent::ToolUse {
                id: "t1".into(),
                name: "Bash".into(),
                input: json!({"command": "ls"}),
            },
            GenEvent::Text { text: "lo".into() },
            GenEvent::Usage {
                prompt_tokens: 3,
                completion_tokens: 2,
            },
            GenEvent::Done {
                finish_reason: "stop".into(),
            },
        ];
        let mut b = ChunkBuilder::new("claude-code/sonnet");
        let chunks: Vec<Value> = evs.iter().filter_map(|e| b.event(e)).collect();
        assert_eq!(chunks.len(), 4);
        assert_eq!(chunks[0]["choices"][0]["delta"]["role"], "assistant");
        assert_eq!(chunks[0]["choices"][0]["delta"]["content"], "Hel");
        assert!(chunks[2]["choices"][0]["delta"].get("role").is_none());
        assert_eq!(chunks[1]["ko_tool_activity"]["name"], "Bash");
        assert!(chunks[1]["choices"][0]["delta"].get("tool_calls").is_none());
        assert_eq!(chunks[3]["choices"][0]["finish_reason"], "stop");
        assert_eq!(chunks[3]["usage"]["total_tokens"], 5);
        assert_eq!(chunks[0]["model"], "claude-code/sonnet");

        let full = aggregate("claude-code/sonnet", &evs).unwrap();
        assert_eq!(full["object"], "chat.completion");
        assert_eq!(full["choices"][0]["message"]["content"], "Hello");
        assert_eq!(full["usage"]["prompt_tokens"], 3);
        assert_eq!(full["ko_tool_activity"].as_array().unwrap().len(), 1);

        let err = aggregate(
            "m",
            &[GenEvent::Error {
                message: "boom".into(),
            }],
        )
        .unwrap_err();
        assert_eq!(err.status, axum::http::StatusCode::BAD_GATEWAY);
        assert!(aggregate("m", &[GenEvent::Text { text: "x".into() }]).is_err());
    }
}
