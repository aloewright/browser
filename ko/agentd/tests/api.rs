// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

//! Workspaces, artifacts, MCP registry, models.

mod common;

use base64::Engine;
use common::*;
use reqwest::Method;
use serde_json::json;

#[tokio::test]
async fn models_lists_configured_providers() {
    let mut env = TestEnv::start("http://127.0.0.1:9", "").await;
    let (s, v) = env.get("/v1/models").await;
    assert_eq!(s, 200);
    assert_eq!(v["object"], "list");
    let data = v["data"].as_array().unwrap();
    let ids: Vec<&str> = data.iter().map(|m| m["id"].as_str().unwrap()).collect();
    assert_eq!(
        ids,
        [
            "claude-code/sonnet",
            "claude-code/opus",
            "codex/gpt-5-codex",
            "cf-gateway/openai/gpt-4o-mini",
            "nanogpt/chatgpt-4o-latest"
        ]
    );
    // Every entry carries what the browser reads (ko_agentd_models.cc).
    for m in data {
        let name = m["display_name"].as_str().unwrap();
        assert!(!name.is_empty() && name.len() <= 100, "{m}");
        assert!(m["capabilities"]["vision"].is_boolean(), "{m}");
        assert!(m["capabilities"]["tools"].is_boolean(), "{m}");
        let ctx = m["context_length"].as_u64().unwrap();
        assert!((700..=2_000_000).contains(&ctx), "{m}");
        assert_eq!(m["object"], "model");
    }
    assert_eq!(
        data[0],
        json!({
            "id": "claude-code/sonnet", "object": "model", "created": 0,
            "owned_by": "claude-code", "display_name": "sonnet (Claude Code)",
            "capabilities": {"vision": false, "tools": false}, "context_length": 128000
        })
    );
    // CLI models never return tool calls; HTTP models default to tools on.
    for (i, tools) in [(0, false), (1, false), (2, false), (3, true), (4, true)] {
        assert_eq!(data[i]["capabilities"]["tools"], tools, "{}", data[i]);
        assert_eq!(data[i]["capabilities"]["vision"], false, "{}", data[i]);
    }
    env.stop().await;
}

#[tokio::test]
async fn models_apply_configured_model_info() {
    let extra = r#"
[providers.nanogpt.model_info."chatgpt-4o-latest"]
display_name = "GPT-4o via NanoGPT"
vision = true
tools = false
context_length = 64000

[providers.claude-code.model_info.opus]
context_length = 200000
"#;
    let mut env = TestEnv::start("http://127.0.0.1:9", extra).await;
    let (s, v) = env.get("/v1/models").await;
    assert_eq!(s, 200);
    let data = v["data"].as_array().unwrap();
    let nano = data
        .iter()
        .find(|m| m["id"] == "nanogpt/chatgpt-4o-latest")
        .unwrap();
    assert_eq!(nano["display_name"], "GPT-4o via NanoGPT");
    assert_eq!(
        nano["capabilities"],
        json!({"vision": true, "tools": false})
    );
    assert_eq!(nano["context_length"], 64000);
    let opus = data.iter().find(|m| m["id"] == "claude-code/opus").unwrap();
    assert_eq!(opus["display_name"], "opus (Claude Code)");
    assert_eq!(opus["context_length"], 200000);
    assert_eq!(opus["capabilities"]["tools"], false);
    env.stop().await;
}

#[tokio::test]
async fn workspace_crud() {
    let mut env = TestEnv::start("http://127.0.0.1:9", "").await;
    let (s, w) = env
        .post(
            "/v1/workspaces",
            json!({"name": " Research ", "container_id": "ctr-42", "tags": ["a", "b"], "default_model": "claude-code/sonnet", "mcp_servers": ["github"]}),
        )
        .await;
    assert_eq!(s, 201, "{w}");
    let id = w["id"].as_str().unwrap().to_owned();
    assert_eq!(w["name"], "Research");
    assert_eq!(w["container_id"], "ctr-42");
    assert!(w["created_at"].as_str().unwrap().ends_with('Z'));
    env.create_workspace("Shopping").await;

    let (_, l) = env.get("/v1/workspaces").await;
    assert_eq!(l["data"].as_array().unwrap().len(), 2);
    let (_, l) = env.get("/v1/workspaces?tag=a").await;
    assert_eq!(l["data"][0]["id"], id.as_str());
    let (_, l) = env.get("/v1/workspaces?q=shop").await;
    assert_eq!(l["data"].as_array().unwrap().len(), 1);

    let (s, p) = env
        .send(
            Method::PATCH,
            &format!("/v1/workspaces/{id}"),
            Some(json!({"container_id": null, "tags": ["c"]})),
        )
        .await;
    assert_eq!(s, 200, "{p}");
    assert!(p["container_id"].is_null());
    assert_eq!(p["tags"], json!(["c"]));
    assert_eq!(p["name"], "Research");

    // Validation.
    for bad in [
        json!({"name": ""}),
        json!({"name": "x", "default_model": "claude-code/unknown"}),
        json!({"name": "x", "mcp_servers": ["nope"]}),
        json!({"name": "bad\u{0007}"}),
        json!({"name": "x", "tags": ["\n"]}),
    ] {
        let (s, _) = env.post("/v1/workspaces", bad.clone()).await;
        assert!(s == 400 || s == 404, "{bad} -> {s}");
    }

    let (s, a) = env.get(&format!("/v1/workspaces/{id}/agents")).await;
    assert_eq!(s, 200);
    let first = &a["data"][0];
    assert_eq!(first["id"], "claude-code/sonnet");
    assert_eq!(first["supports_mcp"], true);
    assert_eq!(first["default"], true);

    let (s, _) = env
        .send(Method::DELETE, &format!("/v1/workspaces/{id}"), None)
        .await;
    assert_eq!(s, 204);
    let (s, _) = env.get(&format!("/v1/workspaces/{id}")).await;
    assert_eq!(s, 404);
    env.stop().await;
}

#[tokio::test]
async fn artifacts_are_encrypted_at_rest() {
    let mut env = TestEnv::start("http://127.0.0.1:9", "").await;
    let ws = env.create_workspace("arts").await;
    let secret_text = "TOP-SECRET-ARTIFACT-CONTENT";
    let (s, a) = env
        .post(
            &format!("/v1/workspaces/{ws}/artifacts"),
            json!({"name": "notes.md", "mime_type": "text/markdown", "tags": ["draft"], "content_text": secret_text}),
        )
        .await;
    assert_eq!(s, 201, "{a}");
    let aid = a["id"].as_str().unwrap().to_owned();
    assert_eq!(a["size"], secret_text.len());
    let bin = vec![0u8, 1, 2, 255];
    let (s, b) = env
        .post(
            &format!("/v1/workspaces/{ws}/artifacts"),
            json!({"name": "blob.bin", "content_base64": base64::engine::general_purpose::STANDARD.encode(&bin)}),
        )
        .await;
    assert_eq!(s, 201);
    assert_eq!(b["mime_type"], "application/octet-stream");

    let (_, l) = env
        .get(&format!("/v1/workspaces/{ws}/artifacts?q=notes&tag=draft"))
        .await;
    assert_eq!(l["data"].as_array().unwrap().len(), 1);
    let (_, l) = env
        .get(&format!(
            "/v1/workspaces/{ws}/artifacts?mime_type=application/octet-stream"
        ))
        .await;
    assert_eq!(l["data"][0]["name"], "blob.bin");

    let (s, g) = env
        .get(&format!("/v1/workspaces/{ws}/artifacts/{aid}"))
        .await;
    assert_eq!(s, 200);
    let content = base64::engine::general_purpose::STANDARD
        .decode(g["content_base64"].as_str().unwrap())
        .unwrap();
    assert_eq!(content, secret_text.as_bytes());

    let raw = env
        .client
        .get(env.url(&format!("/v1/workspaces/{ws}/artifacts/{aid}?raw=true")))
        .bearer_auth(&env.token)
        .send()
        .await
        .unwrap();
    assert_eq!(raw.headers()["content-type"], "application/octet-stream");
    assert_eq!(raw.headers()["content-disposition"], "attachment");
    assert_eq!(raw.headers()["x-ko-mime-type"], "text/markdown");
    assert_eq!(raw.text().await.unwrap(), secret_text);

    // On disk: ciphertext only, 0600.
    let blob = env
        .paths
        .artifacts_dir()
        .join(&ws)
        .join(format!("{aid}.bin"));
    let bytes = std::fs::read(&blob).unwrap();
    assert!(!String::from_utf8_lossy(&bytes).contains(secret_text));
    assert_eq!(&bytes[..4], b"KOE1");
    use std::os::unix::fs::PermissionsExt;
    assert_eq!(
        std::fs::metadata(&blob).unwrap().permissions().mode() & 0o777,
        0o600
    );
    let db = std::fs::read(env.paths.db()).unwrap();
    assert!(!String::from_utf8_lossy(&db).contains(secret_text));

    // Tampering is detected.
    let mut t = bytes.clone();
    let last = t.len() - 1;
    t[last] ^= 0x55;
    std::fs::write(&blob, &t).unwrap();
    let (s, v) = env
        .get(&format!("/v1/workspaces/{ws}/artifacts/{aid}"))
        .await;
    assert_eq!(s, 500);
    assert!(!v.to_string().contains(secret_text));
    std::fs::write(&blob, &bytes).unwrap();

    // Cross-workspace access is not possible.
    let other = env.create_workspace("other").await;
    let (s, _) = env
        .get(&format!("/v1/workspaces/{other}/artifacts/{aid}"))
        .await;
    assert_eq!(s, 404);

    let (s, _) = env
        .send(
            Method::DELETE,
            &format!("/v1/workspaces/{ws}/artifacts/{aid}"),
            None,
        )
        .await;
    assert_eq!(s, 204);
    assert!(!blob.exists());
    let (s, _) = env
        .post(
            &format!("/v1/workspaces/{ws}/artifacts"),
            json!({"name": "x"}),
        )
        .await;
    assert_eq!(s, 400);
    // Workspace delete removes its blobs.
    let (s, _) = env
        .send(Method::DELETE, &format!("/v1/workspaces/{ws}"), None)
        .await;
    assert_eq!(s, 204);
    assert!(!env.paths.artifacts_dir().join(&ws).exists());
    env.stop().await;
}

#[tokio::test]
async fn mcp_registry() {
    let mut env = TestEnv::start("http://127.0.0.1:9", "").await;
    let (s, l) = env.get("/v1/mcp/servers").await;
    assert_eq!(s, 200);
    let gh = &l["data"][0];
    assert_eq!(gh["name"], "github");
    assert_eq!(gh["source"], "config");
    assert_eq!(gh["env_keys"], json!(["GITHUB_PERSONAL_ACCESS_TOKEN"]));
    assert_eq!(
        gh["secret_refs"][0],
        json!({"name": "mcp-github-token", "set": true})
    );
    assert!(!l.to_string().contains(GH_TOKEN));

    let (s, v) = env
        .post("/v1/mcp/servers", json!({"name": "docs", "type": "http", "url": "https://mcp.example.com/mcp", "bearer_secret": "mcp-docs-token"}))
        .await;
    assert_eq!(s, 201, "{v}");
    assert_eq!(v["source"], "api");
    let (s, _) = env
        .post(
            "/v1/mcp/servers",
            json!({"name": "docs", "type": "http", "url": "https://mcp.example.com/mcp"}),
        )
        .await;
    assert_eq!(s, 409);
    let (s, v) = env
        .post(
            "/v1/mcp/servers",
            json!({"name": "shell", "type": "stdio", "command": "sh", "args": ["-c", "id"]}),
        )
        .await;
    assert_eq!(s, 400, "{v}");
    assert!(
        v["error"]["message"]
            .as_str()
            .unwrap()
            .contains("allow_api_stdio_servers")
    );
    let (s, _) = env
        .post(
            "/v1/mcp/servers",
            json!({"name": "inline", "type": "http", "url": "https://a.example", "token": "x"}),
        )
        .await;
    assert_eq!(s, 400);
    let (s, _) = env
        .post(
            "/v1/mcp/servers",
            json!({"name": "plain", "type": "http", "url": "http://a.example"}),
        )
        .await;
    assert_eq!(s, 400);
    let (s, _) = env
        .send(Method::DELETE, "/v1/mcp/servers/github", None)
        .await;
    assert_eq!(s, 409);
    let (s, _) = env.send(Method::DELETE, "/v1/mcp/servers/docs", None).await;
    assert_eq!(s, 204);
    env.stop().await;
}

#[tokio::test]
async fn api_mcp_servers_cannot_reference_non_mcp_secrets() {
    let mut env = TestEnv::start("http://127.0.0.1:9", "").await;
    for (field, refs) in [
        ("bearer_secret", json!("cloudflare-api-token")),
        ("header_secrets", json!({"X-Key": "nanogpt-api-key"})),
        ("bearer_secret", json!("storage-key")),
    ] {
        let mut body =
            json!({"name": "exfil", "type": "http", "url": "https://mcp.example.com/mcp"});
        body[field] = refs;
        let (s, v) = env.post("/v1/mcp/servers", body).await;
        assert_eq!(s, 400, "{v}");
        assert!(
            v["error"]["message"].as_str().unwrap().contains("mcp-"),
            "{v}"
        );
    }
    // Nothing was registered, so no run can send these secrets anywhere.
    let ws = env.create_workspace("exfil").await;
    let (s, _) = env
        .post(
            &format!("/v1/workspaces/{ws}/runs"),
            json!({"model": "claude-code/sonnet", "prompt": "x", "mcp_servers": ["exfil"]}),
        )
        .await;
    assert_eq!(s, 400);
    assert!(!env.fake_file("claude.mcp.json").contains(CF_API_TOKEN));
    env.stop().await;
}
