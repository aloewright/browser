// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

//! /v1/chat/completions against mock upstreams and fake CLIs.

mod common;

use common::*;
use serde_json::{Value, json};
use wiremock::matchers::{header, method, path};
use wiremock::{Mock, MockServer, ResponseTemplate};

fn completion(model: &str) -> Value {
    json!({
        "id": "chatcmpl-up", "object": "chat.completion", "created": 1, "model": model,
        "choices": [{"index": 0, "message": {"role": "assistant", "content": null,
            "tool_calls": [{"id": "call_1", "type": "function", "function": {"name": "get_weather", "arguments": "{\"city\":\"Paris\"}"}}]},
            "finish_reason": "tool_calls"}],
        "usage": {"prompt_tokens": 5, "completion_tokens": 7, "total_tokens": 12}
    })
}

const SSE_BODY: &str = "data: {\"id\":\"c\",\"object\":\"chat.completion.chunk\",\"model\":\"up\",\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"Hel\"},\"finish_reason\":null}]}\n\n\
data: {\"id\":\"c\",\"object\":\"chat.completion.chunk\",\"model\":\"up\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"lo\"},\"finish_reason\":\"stop\"}]}\n\n\
data: [DONE]\n\n";

fn tools_request(model: &str, stream: bool) -> Value {
    json!({
        "model": model, "stream": stream,
        "messages": [{"role": "user", "content": "weather in Paris?"}],
        "tools": [{"type": "function", "function": {"name": "get_weather", "parameters": {"type": "object"}}}]
    })
}

#[tokio::test]
async fn nanogpt_passthrough_with_tools() {
    let mock = MockServer::start().await;
    Mock::given(method("POST"))
        .and(path("/api/v1/chat/completions"))
        .and(header(
            "authorization",
            format!("Bearer {NANO_KEY}").as_str(),
        ))
        .respond_with(ResponseTemplate::new(200).set_body_json(completion("chatgpt-4o-latest")))
        .expect(1)
        .mount(&mock)
        .await;
    let mut env = TestEnv::start(&mock.uri(), "").await;
    let (s, v) = env
        .post(
            "/v1/chat/completions",
            tools_request("nanogpt/chatgpt-4o-latest", false),
        )
        .await;
    assert_eq!(s, 200, "{v}");
    assert_eq!(v["model"], "nanogpt/chatgpt-4o-latest");
    assert_eq!(
        v["choices"][0]["message"]["tool_calls"][0]["function"]["name"],
        "get_weather"
    );
    let reqs = mock.received_requests().await.unwrap();
    let body: Value = serde_json::from_slice(&reqs[0].body).unwrap();
    assert_eq!(body["model"], "chatgpt-4o-latest");
    assert_eq!(body["tools"][0]["function"]["name"], "get_weather");
    assert_eq!(body["stream"], false);
    env.stop().await;
}

#[tokio::test]
async fn cf_gateway_streams_and_sends_gateway_auth() {
    let mock = MockServer::start().await;
    Mock::given(method("POST"))
        .and(path(format!("/v1/{ACCOUNT}/ko-gw/compat/chat/completions")))
        .and(header(
            "cf-aig-authorization",
            format!("Bearer {CF_AIG}").as_str(),
        ))
        .and(header(
            "authorization",
            format!("Bearer {CF_OPENAI_KEY}").as_str(),
        ))
        .respond_with(
            ResponseTemplate::new(200)
                .insert_header("content-type", "text/event-stream")
                .set_body_string(SSE_BODY),
        )
        .expect(1)
        .mount(&mock)
        .await;
    let mut env = TestEnv::start(&mock.uri(), "").await;
    let ev = env
        .sse(
            "/v1/chat/completions",
            Some(tools_request("cf-gateway/openai/gpt-4o-mini", true)),
        )
        .await;
    assert_eq!(ev.len(), 3);
    let c0: Value = serde_json::from_str(&ev[0].1).unwrap();
    assert_eq!(c0["model"], "cf-gateway/openai/gpt-4o-mini");
    assert_eq!(c0["choices"][0]["delta"]["content"], "Hel");
    assert_eq!(ev[2].1, "[DONE]");
    let reqs = mock.received_requests().await.unwrap();
    let body: Value = serde_json::from_slice(&reqs[0].body).unwrap();
    assert_eq!(body["model"], "openai/gpt-4o-mini");
    assert_eq!(body["stream"], true);
    env.stop().await;
}

#[tokio::test]
async fn upstream_errors_do_not_leak_keys() {
    let mock = MockServer::start().await;
    Mock::given(method("POST"))
        .and(path("/api/v1/chat/completions"))
        .respond_with(ResponseTemplate::new(401).set_body_json(
            json!({"error": {"message": format!("Incorrect API key provided: {NANO_KEY}")}}),
        ))
        .mount(&mock)
        .await;
    let mut env = TestEnv::start(&mock.uri(), "").await;
    let (s, v) = env
        .post("/v1/chat/completions", json!({"model": "nanogpt/chatgpt-4o-latest", "messages": [{"role": "user", "content": "x"}]}))
        .await;
    assert_eq!(s, 502, "{v}");
    assert_eq!(v["error"]["type"], "provider_error");
    assert!(!v.to_string().contains(NANO_KEY), "{v}");
    assert!(v.to_string().contains("401"));
    // Unknown and unconfigured models.
    let (s, v) = env
        .post(
            "/v1/chat/completions",
            json!({"model": "nanogpt/other", "messages": []}),
        )
        .await;
    assert_eq!(s, 404);
    assert_eq!(v["error"]["type"], "model_not_found");
    env.stop().await;
}

#[tokio::test]
async fn missing_provider_key_fails_closed() {
    let mock = MockServer::start().await;
    let mut env = TestEnv::start(&mock.uri(), "").await;
    let store = ko_agentd::secrets::SecretStore::open(
        ko_agentd::config::SecretBackendKind::File,
        &env.paths,
    )
    .unwrap();
    store.delete("nanogpt-api-key").unwrap();
    let (s, v) = env
        .post("/v1/chat/completions", json!({"model": "nanogpt/chatgpt-4o-latest", "messages": [{"role": "user", "content": "x"}]}))
        .await;
    assert_eq!(s, 503, "{v}");
    assert!(mock.received_requests().await.unwrap().is_empty());
    env.stop().await;
}

#[tokio::test]
async fn claude_code_cli_streams_openai_chunks() {
    let mut env = TestEnv::start("http://127.0.0.1:9", "").await;
    let body = json!({
        "model": "claude-code/sonnet", "stream": true,
        "messages": [{"role": "system", "content": "be brief"}, {"role": "user", "content": "list files"}]
    });
    let ev = env.sse("/v1/chat/completions", Some(body)).await;
    let chunks: Vec<Value> = ev
        .iter()
        .filter(|(_, d)| d != "[DONE]")
        .map(|(_, d)| serde_json::from_str(d).unwrap())
        .collect();
    assert_eq!(ev.last().unwrap().1, "[DONE]");
    let text: String = chunks
        .iter()
        .filter_map(|c| c["choices"][0]["delta"]["content"].as_str())
        .collect();
    assert_eq!(
        text,
        "I'll list the files.\n\nThe directory contains README.md and src."
    );
    assert_eq!(chunks[0]["choices"][0]["delta"]["role"], "assistant");
    assert_eq!(chunks[0]["object"], "chat.completion.chunk");
    let tools: Vec<&Value> = chunks
        .iter()
        .filter_map(|c| c.get("ko_tool_activity"))
        .collect();
    assert_eq!(tools.len(), 2);
    assert_eq!(tools[0]["name"], "Bash");
    let last = chunks.last().unwrap();
    assert_eq!(last["choices"][0]["finish_reason"], "stop");
    assert_eq!(last["usage"]["completion_tokens"], 42);

    // argv: headless flags, model, no prompt; prompt arrived on stdin.
    let args = env.fake_file("claude.args");
    let args: Vec<&str> = args.lines().collect();
    assert_eq!(
        args,
        [
            "-p",
            "--output-format",
            "stream-json",
            "--verbose",
            "--model",
            "sonnet"
        ]
    );
    assert_eq!(
        env.fake_file("claude.stdin"),
        "[System]\nbe brief\n\n[User]\nlist files"
    );
    // Minimal env with a private TMPDIR inside the data dir; no secrets.
    let envs = env.fake_file("claude.env");
    let tmp_root = env.paths.tmp_dir().display().to_string();
    assert!(
        envs.lines()
            .any(|l| l.starts_with("TMPDIR=") && l.contains(&tmp_root)),
        "{envs}"
    );
    for s in [NANO_KEY, CF_AIG, GH_TOKEN, &env.token] {
        assert!(!envs.contains(s));
    }
    assert!(!envs.contains("ANTHROPIC_API_KEY"));
    // Per-request dir removed afterwards.
    assert!(
        eventually(|| std::fs::read_dir(env.paths.tmp_dir())
            .unwrap()
            .next()
            .is_none())
        .await
    );
    env.stop().await;
}

#[tokio::test]
async fn claude_code_non_stream_and_errors() {
    let mut env = TestEnv::start("http://127.0.0.1:9", "").await;
    let (s, v) = env
        .post(
            "/v1/chat/completions",
            json!({"model": "claude-code/opus", "messages": [{"role": "user", "content": "hi"}]}),
        )
        .await;
    assert_eq!(s, 200, "{v}");
    assert_eq!(v["object"], "chat.completion");
    assert_eq!(
        v["choices"][0]["message"]["content"],
        "I'll list the files.\n\nThe directory contains README.md and src."
    );
    assert_eq!(v["usage"]["total_tokens"], 1292);
    assert_eq!(v["ko_tool_activity"].as_array().unwrap().len(), 2);

    let (s, v) = env
        .post(
            "/v1/chat/completions",
            json!({"model": "claude-code/opus", "messages": [{"role": "user", "content": "FAIL"}]}),
        )
        .await;
    assert_eq!(s, 502);
    assert!(
        v["error"]["message"]
            .as_str()
            .unwrap()
            .contains("model refused")
    );

    let (s, v) = env
        .post("/v1/chat/completions", json!({"model": "claude-code/opus", "messages": [{"role": "user", "content": "CRASH"}]}))
        .await;
    assert_eq!(s, 502);
    let msg = v["error"]["message"].as_str().unwrap();
    assert!(msg.contains("status 3"), "{msg}");
    assert!(!msg.contains("shouldbescrubbed"), "{msg}");

    let ev = env
        .sse("/v1/chat/completions", Some(json!({"model": "claude-code/opus", "stream": true, "messages": [{"role": "user", "content": "FAIL"}]})))
        .await;
    let err: Value = serde_json::from_str(&ev[0].1).unwrap();
    assert!(
        err["error"]["message"]
            .as_str()
            .unwrap()
            .contains("model refused")
    );
    assert_eq!(ev.last().unwrap().1, "[DONE]");

    let (s, _) = env
        .post("/v1/chat/completions", json!({"model": "claude-code/opus", "messages": [{"role": "user", "content": [{"type": "image_url"}]}]}))
        .await;
    assert_eq!(s, 400);
    env.stop().await;
}

#[tokio::test]
async fn codex_cli_chat() {
    let mut env = TestEnv::start("http://127.0.0.1:9", "").await;
    let (s, v) = env
        .post("/v1/chat/completions", json!({"model": "codex/gpt-5-codex", "messages": [{"role": "user", "content": "describe repo"}]}))
        .await;
    assert_eq!(s, 200, "{v}");
    assert_eq!(
        v["choices"][0]["message"]["content"],
        "Repo contains docs, sdk, and examples directories."
    );
    assert_eq!(v["usage"]["prompt_tokens"], 24763);
    let args = env.fake_file("codex.args");
    let args: Vec<&str> = args.lines().collect();
    assert_eq!(
        args,
        [
            "exec",
            "--json",
            "--skip-git-repo-check",
            "--sandbox",
            "read-only",
            "--model",
            "gpt-5-codex",
            "--ephemeral",
            "-"
        ]
    );
    assert_eq!(env.fake_file("codex.stdin"), "describe repo");
    let (s, v) = env
        .post("/v1/chat/completions", json!({"model": "codex/gpt-5-codex", "messages": [{"role": "user", "content": "FAIL"}]}))
        .await;
    assert_eq!(s, 502);
    assert!(
        v["error"]["message"]
            .as_str()
            .unwrap()
            .contains("usage limit")
    );
    env.stop().await;
}

#[tokio::test]
async fn http_stream_cut_short_reports_an_error() {
    let mock = MockServer::start().await;
    // Content but no finish_reason and no [DONE]: the upstream hung up early.
    Mock::given(method("POST"))
        .and(path("/api/v1/chat/completions"))
        .respond_with(
            ResponseTemplate::new(200)
                .insert_header("content-type", "text/event-stream")
                .set_body_string(
                    "data: {\"id\":\"c\",\"object\":\"chat.completion.chunk\",\"model\":\"up\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Hel\"},\"finish_reason\":null}]}\n\n",
                ),
        )
        .mount(&mock)
        .await;
    let mut env = TestEnv::start(&mock.uri(), "").await;
    let body = json!({"model": "nanogpt/chatgpt-4o-latest", "stream": true,
        "messages": [{"role": "user", "content": "x"}]});
    let ev = env.sse("/v1/chat/completions", Some(body)).await;
    assert_eq!(ev.last().unwrap().1, "[DONE]");
    let chunks: Vec<Value> = ev
        .iter()
        .filter(|(_, d)| d != "[DONE]")
        .map(|(_, d)| serde_json::from_str(d).unwrap())
        .collect();
    assert_eq!(chunks.len(), 2, "{chunks:?}");
    assert_eq!(chunks[0]["choices"][0]["delta"]["content"], "Hel");
    assert!(
        chunks[1]["error"]["message"]
            .as_str()
            .unwrap()
            .contains("before finishing"),
        "{chunks:?}"
    );
    env.stop().await;
}

#[tokio::test]
async fn http_stream_finished_without_done_is_complete() {
    let mock = MockServer::start().await;
    // finish_reason arrived; the missing [DONE] alone is not an error.
    Mock::given(method("POST"))
        .and(path("/api/v1/chat/completions"))
        .respond_with(
            ResponseTemplate::new(200)
                .insert_header("content-type", "text/event-stream")
                .set_body_string(SSE_BODY.trim_end_matches("data: [DONE]\n\n")),
        )
        .mount(&mock)
        .await;
    let mut env = TestEnv::start(&mock.uri(), "").await;
    let body = json!({"model": "nanogpt/chatgpt-4o-latest", "stream": true,
        "messages": [{"role": "user", "content": "x"}]});
    let ev = env.sse("/v1/chat/completions", Some(body)).await;
    assert!(ev.iter().all(|(_, d)| !d.contains("\"error\"")), "{ev:?}");
    assert_eq!(ev.last().unwrap().1, "[DONE]");
    env.stop().await;
}
