// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

//! Renders OpenAI chat messages into a single prompt for CLI providers.

use serde_json::Value;

use crate::error::ApiError;

pub const MAX_MESSAGES: usize = 512;

fn content_text(content: &Value) -> Result<String, ApiError> {
    match content {
        Value::Null => Ok(String::new()),
        Value::String(s) => Ok(s.clone()),
        Value::Array(parts) => {
            let mut out = Vec::new();
            for p in parts {
                match p.get("type").and_then(Value::as_str) {
                    Some("text") => out.push(
                        p.get("text")
                            .and_then(Value::as_str)
                            .unwrap_or("")
                            .to_owned(),
                    ),
                    Some(t) => {
                        return Err(ApiError::bad_request(format!(
                            "content part type {:?} is not supported by CLI providers",
                            crate::util::truncate(t, 40)
                        )));
                    }
                    None => return Err(ApiError::bad_request("content part has no type")),
                }
            }
            Ok(out.join("\n"))
        }
        _ => Err(ApiError::bad_request(
            "message content must be a string or array",
        )),
    }
}

/// A lone user message is passed verbatim; otherwise roles are labelled.
pub fn render(messages: &[Value]) -> Result<String, ApiError> {
    if messages.is_empty() {
        return Err(ApiError::bad_request("messages must not be empty"));
    }
    if messages.len() > MAX_MESSAGES {
        return Err(ApiError::bad_request("too many messages"));
    }
    let mut parts: Vec<(String, String)> = Vec::with_capacity(messages.len());
    for m in messages {
        let role = m
            .get("role")
            .and_then(Value::as_str)
            .ok_or_else(|| ApiError::bad_request("message has no role"))?;
        let label = match role {
            "system" | "developer" => "System",
            "user" => "User",
            "assistant" => "Assistant",
            "tool" => "Tool result",
            _ => return Err(ApiError::bad_request("unsupported message role")),
        };
        let text = content_text(m.get("content").unwrap_or(&Value::Null))?;
        parts.push((label.to_owned(), text));
    }
    if parts.last().map(|(l, _)| l.as_str()) != Some("User") {
        return Err(ApiError::bad_request(
            "the last message must be from the user",
        ));
    }
    if parts.len() == 1 {
        return Ok(parts.pop().map(|(_, t)| t).unwrap_or_default());
    }
    Ok(parts
        .iter()
        .map(|(l, t)| format!("[{l}]\n{t}"))
        .collect::<Vec<_>>()
        .join("\n\n"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn renders() {
        assert_eq!(
            render(&[json!({"role":"user","content":"hi"})]).unwrap(),
            "hi"
        );
        let p = render(&[
            json!({"role":"system","content":"be brief"}),
            json!({"role":"user","content":[{"type":"text","text":"a"},{"type":"text","text":"b"}]}),
            json!({"role":"assistant","content":"ok"}),
            json!({"role":"user","content":"next"}),
        ])
        .unwrap();
        assert_eq!(
            p,
            "[System]\nbe brief\n\n[User]\na\nb\n\n[Assistant]\nok\n\n[User]\nnext"
        );
        assert!(render(&[]).is_err());
        assert!(render(&[json!({"role":"assistant","content":"x"})]).is_err());
        assert!(
            render(&[json!({"role":"user","content":[{"type":"image_url","image_url":{}}]})])
                .is_err()
        );
    }
}
