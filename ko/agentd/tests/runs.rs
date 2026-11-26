// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

//! Agent runs: MCP config generation, encrypted event logs, SSE, cancel.

mod common;

use std::time::Duration;

use common::*;
use reqwest::Method;
use serde_json::{Value, json};

async fn wait_done(env: &TestEnv, rid: &str) -> Value {
    for _ in 0..200 {
        let (_, r) = env.get(&format!("/v1/runs/{rid}")).await;
        if matches!(
            r["status"].as_str(),
            Some("succeeded" | "failed" | "cancelled")
        ) {
            return r;
        }
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
    panic!("run {rid} did not finish");
}

#[tokio::test]
async fn claude_run_with_mcp_servers() {
    let mut env = TestEnv::start("http://127.0.0.1:9", "").await;
    let (s, _) = env
        .post("/v1/mcp/servers", json!({"name": "docs", "type": "http", "url": "https://mcp.example.com/mcp", "bearer_secret": "mcp-docs-token"}))
        .await;
    assert_eq!(s, 201);
    let ws = env.create_workspace("agents").await;
    let (s, run) = env
        .post(
            &format!("/v1/workspaces/{ws}/runs"),
            json!({"provider": "claude-code", "model": "sonnet", "prompt": "SECRET-PROMPT list files", "mcp_servers": ["github", "docs"]}),
        )
        .await;
    assert_eq!(s, 202, "{run}");
    let rid = run["id"].as_str().unwrap().to_owned();
    assert_eq!(run["model"], "claude-code/sonnet");
    assert_eq!(run["mcp_servers"], json!(["github", "docs"]));

    let ev = env.sse(&format!("/v1/runs/{rid}/events"), None).await;
    let kinds: Vec<&str> = ev.iter().map(|(k, _)| k.as_str()).collect();
    assert_eq!(
        kinds,
        [
            "status",
            "status",
            "text",
            "tool_use",
            "tool_result",
            "text",
            "usage",
            "done"
        ]
    );
    let done: Value = serde_json::from_str(&ev.last().unwrap().1).unwrap();
    assert_eq!(done["data"]["status"], "succeeded");
    let seqs: Vec<u64> = ev
        .iter()
        .map(|(_, d)| {
            serde_json::from_str::<Value>(d).unwrap()["seq"]
                .as_u64()
                .unwrap()
        })
        .collect();
    assert_eq!(seqs, (1..=8).collect::<Vec<_>>());

    let r = wait_done(&env, &rid).await;
    assert_eq!(r["status"], "succeeded");
    assert_eq!(
        r["output"],
        "I'll list the files.\n\nThe directory contains README.md and src."
    );
    assert_eq!(r["usage"]["completion_tokens"], 42);

    // Agent-run flags and a strict per-run MCP config with secrets resolved.
    let args = env.fake_file("claude.args");
    assert!(args.contains("--mcp-config\n"));
    assert!(args.contains("--strict-mcp-config"));
    assert!(args.contains("--permission-mode\nacceptEdits"));
    assert!(!args.contains("SECRET-PROMPT"));
    assert!(!args.contains(GH_TOKEN) && !args.contains(MCP_TOKEN));
    assert_eq!(env.fake_file("claude.mcp_mode").trim(), "600");
    assert_eq!(env.fake_file("claude.mcp_dir_mode").trim(), "700");
    let mcp: Value = serde_json::from_str(&env.fake_file("claude.mcp.json")).unwrap();
    assert_eq!(
        mcp["mcpServers"]["github"]["env"]["GITHUB_PERSONAL_ACCESS_TOKEN"],
        GH_TOKEN
    );
    assert_eq!(
        mcp["mcpServers"]["docs"]["headers"]["Authorization"],
        format!("Bearer {MCP_TOKEN}")
    );
    let mcp_path = env.fake_file("claude.mcp_path");
    assert!(
        !std::path::Path::new(mcp_path.trim()).exists(),
        "MCP config deleted"
    );
    assert!(
        eventually(|| std::fs::read_dir(env.paths.tmp_dir())
            .unwrap()
            .next()
            .is_none())
        .await
    );
    // Runs execute in the workspace work dir.
    let cwd = env.fake_file("claude.cwd");
    assert!(
        cwd.trim().ends_with(&format!("workspaces/{ws}/work")),
        "{cwd}"
    );

    // Event log is encrypted at rest.
    let log = std::fs::read(env.paths.runs_dir().join(format!("{rid}.log"))).unwrap();
    let log = String::from_utf8_lossy(&log);
    assert!(!log.contains("README") && !log.contains("list the files"));

    // Replay after Last-Event-ID.
    let r = env
        .client
        .get(env.url(&format!("/v1/runs/{rid}/events")))
        .bearer_auth(&env.token)
        .header("last-event-id", "6")
        .send()
        .await
        .unwrap();
    let ev = parse_sse(&r.text().await.unwrap());
    assert_eq!(
        ev.iter().map(|(k, _)| k.as_str()).collect::<Vec<_>>(),
        ["usage", "done"]
    );

    let (_, list) = env.get(&format!("/v1/workspaces/{ws}/runs")).await;
    assert_eq!(list["data"].as_array().unwrap().len(), 1);
    env.stop().await;
}

#[tokio::test]
async fn codex_run_passes_mcp_secrets_by_env_only() {
    let mut env = TestEnv::start("http://127.0.0.1:9", "").await;
    let ws = env.create_workspace("codex").await;
    let (s, run) = env
        .post(
            &format!("/v1/workspaces/{ws}/runs"),
            json!({"model": "codex/gpt-5-codex", "prompt": "go", "mcp_servers": ["github"]}),
        )
        .await;
    assert_eq!(s, 202, "{run}");
    let r = wait_done(&env, run["id"].as_str().unwrap()).await;
    assert_eq!(r["status"], "succeeded", "{r}");
    let args = env.fake_file("codex.args");
    assert!(args.contains("--sandbox\nworkspace-write"));
    assert!(args.contains("mcp_servers.github.command=\"npx\""));
    assert!(args.contains("mcp_servers.github.env_vars=[\"GITHUB_PERSONAL_ACCESS_TOKEN\"]"));
    assert!(!args.contains(GH_TOKEN));
    let envs = env.fake_file("codex.env");
    assert!(envs.contains(&format!("GITHUB_PERSONAL_ACCESS_TOKEN={GH_TOKEN}")));
    env.stop().await;
}

#[tokio::test]
async fn runs_fail_cancel_and_validate() {
    let mut env = TestEnv::start("http://127.0.0.1:9", "").await;
    let ws = env.create_workspace("ops").await;
    let start = |prompt: &str, extra: Value| {
        let mut b = json!({"model": "claude-code/sonnet", "prompt": prompt});
        if let (Some(o), Some(e)) = (b.as_object_mut(), extra.as_object()) {
            o.extend(e.clone());
        }
        b
    };
    // Failure.
    let (_, run) = env
        .post(
            &format!("/v1/workspaces/{ws}/runs"),
            start("FAIL", json!({})),
        )
        .await;
    let r = wait_done(&env, run["id"].as_str().unwrap()).await;
    assert_eq!(r["status"], "failed");
    assert!(r["error"].as_str().unwrap().contains("model refused"));

    // Cancel kills the CLI process group.
    std::fs::remove_file(env.fakes.join("claude.pid")).unwrap();
    std::fs::remove_file(env.fakes.join("claude.stdin")).unwrap();
    let (_, run) = env
        .post(
            &format!("/v1/workspaces/{ws}/runs"),
            start("SLEEP", json!({})),
        )
        .await;
    let rid = run["id"].as_str().unwrap().to_owned();
    assert!(eventually(|| env.fake_file("claude.stdin").contains("SLEEP")).await);
    let pid: i32 = env.fake_file("claude.pid").trim().parse().unwrap();
    tokio::time::sleep(Duration::from_millis(200)).await;
    let (s, _) = env
        .send(Method::POST, &format!("/v1/runs/{rid}/cancel"), None)
        .await;
    assert_eq!(s, 202);
    let r = wait_done(&env, &rid).await;
    assert_eq!(r["status"], "cancelled");
    // SAFETY: signal 0 only checks for existence.
    assert!(
        eventually(|| unsafe { libc::kill(pid, 0) } != 0).await,
        "child still alive"
    );
    let (s, _) = env
        .send(Method::POST, &format!("/v1/runs/{rid}/cancel"), None)
        .await;
    assert_eq!(s, 409);

    // Validation.
    for (body, want) in [
        (start("", json!({})), 400),
        (json!({"prompt": "x"}), 400),
        (
            json!({"model": "nanogpt/chatgpt-4o-latest", "prompt": "x", "mcp_servers": ["github"]}),
            400,
        ),
        (start("x", json!({"mcp_servers": ["unknown"]})), 400),
        (json!({"model": "claude-code/nope", "prompt": "x"}), 404),
    ] {
        let (s, v) = env
            .post(&format!("/v1/workspaces/{ws}/runs"), body.clone())
            .await;
        assert_eq!(s, want, "{body} -> {v}");
    }
    let (s, _) = env
        .get("/v1/runs/00000000-0000-4000-8000-000000000000")
        .await;
    assert_eq!(s, 404);

    // Tampered log is rejected.
    let rid = run["id"].as_str().unwrap();
    let p = env.paths.runs_dir().join(format!("{rid}.log"));
    let mut b = std::fs::read(&p).unwrap();
    b[10] ^= 1;
    std::fs::write(&p, b).unwrap();
    let (s, _) = env.get(&format!("/v1/runs/{rid}")).await;
    assert_eq!(s, 500);
    env.stop().await;
}

#[tokio::test]
async fn http_provider_runs_and_restart_marks_interrupted() {
    use wiremock::matchers::{method, path};
    use wiremock::{Mock, MockServer, ResponseTemplate};
    let mock = MockServer::start().await;
    Mock::given(method("POST"))
        .and(path("/api/v1/chat/completions"))
        .respond_with(ResponseTemplate::new(200).insert_header("content-type", "text/event-stream").set_body_string(
            "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"},\"finish_reason\":\"stop\"}]}\n\n\
             data: {\"choices\":[],\"usage\":{\"prompt_tokens\":1,\"completion_tokens\":1}}\n\ndata: [DONE]\n\n",
        ))
        .mount(&mock)
        .await;
    let mut env = TestEnv::start(&mock.uri(), "").await;
    let ws = env.create_workspace("http").await;
    let (_, run) = env
        .post(
            &format!("/v1/workspaces/{ws}/runs"),
            json!({"model": "nanogpt/chatgpt-4o-latest", "prompt": "hello"}),
        )
        .await;
    let r = wait_done(&env, run["id"].as_str().unwrap()).await;
    assert_eq!(r["status"], "succeeded", "{r}");
    assert_eq!(r["output"], "hi");

    // A run active at shutdown is recorded as cancelled, not lost.
    let (_, run) = env
        .post(
            &format!("/v1/workspaces/{ws}/runs"),
            json!({"model": "claude-code/sonnet", "prompt": "SLEEP"}),
        )
        .await;
    let rid = run["id"].as_str().unwrap().to_owned();
    assert!(eventually(|| env.fake_file("claude.stdin").contains("SLEEP")).await);
    env.stop().await;
    let db = rusqlite::Connection::open(env.paths.db()).unwrap();
    let status: String = db
        .query_row("SELECT status FROM runs WHERE id=?1", [&rid], |r| r.get(0))
        .unwrap();
    assert_eq!(status, "cancelled");
}
