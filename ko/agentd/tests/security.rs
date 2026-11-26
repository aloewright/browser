// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

//! Auth, Host (DNS rebinding), Origin/CORS, limits, file permissions.

mod common;

use std::os::unix::fs::PermissionsExt;

use common::*;
use serde_json::json;

fn req(port: u16, extra: &str, path: &str, token: Option<&str>) -> String {
    let auth = token
        .map(|t| format!("Authorization: Bearer {t}\r\n"))
        .unwrap_or_default();
    format!(
        "GET {path} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\n{auth}{extra}Connection: close\r\n\r\n"
    )
}

#[tokio::test]
async fn bearer_token_required() {
    let mut env = TestEnv::start("http://127.0.0.1:9", "").await;
    let p = env.port();
    let (s, _, body) = raw_tcp(p, &req(p, "", "/v1/health", None)).await;
    assert_eq!(s, 200);
    assert!(body.contains("\"ok\""));
    let (s, head, body) = raw_tcp(p, &req(p, "", "/v1/models", None)).await;
    assert_eq!(s, 401);
    assert!(head.to_lowercase().contains("www-authenticate: bearer"));
    assert!(body.contains("authentication_error"));
    let (s, _, _) = raw_tcp(
        p,
        &req(
            p,
            "",
            "/v1/models",
            Some("wrong-token-wrong-token-wrong-token"),
        ),
    )
    .await;
    assert_eq!(s, 401);
    let (s, _, _) = raw_tcp(
        p,
        &req(
            p,
            "Authorization: Basic Zm9vOmJhcg==\r\n",
            "/v1/models",
            None,
        ),
    )
    .await;
    assert_eq!(s, 401);
    let (s, head, _) = raw_tcp(p, &req(p, "", "/v1/models", Some(&env.token))).await;
    assert_eq!(s, 200);
    let h = head.to_lowercase();
    for want in [
        "x-content-type-options: nosniff",
        "cache-control: no-store",
        "content-security-policy: default-src 'none'",
    ] {
        assert!(h.contains(want), "missing {want}");
    }
    // Token file and data dir are owner-only.
    let mode = |p: &std::path::Path| std::fs::metadata(p).unwrap().permissions().mode() & 0o777;
    assert_eq!(mode(&env.paths.token()), 0o600);
    assert_eq!(mode(env.paths.root()), 0o700);
    assert_eq!(mode(&env.paths.db()), 0o600);
    assert_eq!(mode(&env.paths.socket()), 0o600);
    env.stop().await;
}

#[tokio::test]
async fn host_header_guard_blocks_dns_rebinding() {
    let mut env = TestEnv::start("http://127.0.0.1:9", "").await;
    let p = env.port();
    let t = env.token.clone();
    for host in [
        "evil.example",
        &format!("evil.example:{p}"),
        "127.0.0.1",
        "127.0.0.1:1",
        "[::1]:1",
    ] {
        let r = format!(
            "GET /v1/health HTTP/1.1\r\nHost: {host}\r\nAuthorization: Bearer {t}\r\nConnection: close\r\n\r\n"
        );
        let (s, _, body) = raw_tcp(p, &r).await;
        assert_eq!(s, 403, "host {host}");
        assert!(body.contains("host not allowed"));
    }
    let ok = format!(
        "GET /v1/models HTTP/1.1\r\nHost: localhost:{p}\r\nAuthorization: Bearer {t}\r\nConnection: close\r\n\r\n"
    );
    assert_eq!(raw_tcp(p, &ok).await.0, 200);
    // Absolute-form request target naming another host.
    let abs = format!(
        "GET http://evil.example:{p}/v1/models HTTP/1.1\r\nHost: 127.0.0.1:{p}\r\nAuthorization: Bearer {t}\r\nConnection: close\r\n\r\n"
    );
    assert_eq!(raw_tcp(p, &abs).await.0, 403);
    // Missing Host.
    let none = format!("GET /v1/models HTTP/1.0\r\nAuthorization: Bearer {t}\r\n\r\n");
    assert_eq!(raw_tcp(p, &none).await.0, 403);
    env.stop().await;
}

#[tokio::test]
async fn cors_only_for_ko_workspace_origin() {
    let mut env = TestEnv::start("http://127.0.0.1:9", "").await;
    let p = env.port();
    let t = env.token.clone();
    {
        let origin = "chrome://ko-workspace";
        let (s, head, _) = raw_tcp(
            p,
            &req(
                p,
                &format!("Origin: {origin}\r\nSec-Fetch-Site: cross-site\r\n"),
                "/v1/models",
                Some(&t),
            ),
        )
        .await;
        assert_eq!(s, 200, "{origin}");
        assert!(
            head.contains(&format!("access-control-allow-origin: {origin}")),
            "{head}"
        );
        assert!(head.contains("access-control-expose-headers: x-ko-mime-type"));
        let pre = format!(
            "OPTIONS /v1/workspaces HTTP/1.1\r\nHost: 127.0.0.1:{p}\r\nOrigin: {origin}\r\n\
             Access-Control-Request-Method: POST\r\nAccess-Control-Request-Headers: authorization, content-type\r\nConnection: close\r\n\r\n"
        );
        let (s, head, _) = raw_tcp(p, &pre).await;
        assert_eq!(s, 204);
        let h = head.to_lowercase();
        assert!(h.contains("access-control-allow-methods:"));
        assert!(h.contains("access-control-allow-headers: authorization, content-type"));
        assert!(!h.contains("access-control-allow-credentials"));
        assert!(!h.contains("access-control-allow-private-network"));
        let pna = format!(
            "OPTIONS /v1/models HTTP/1.1\r\nHost: 127.0.0.1:{p}\r\nOrigin: {origin}\r\n\
             Access-Control-Request-Method: GET\r\nAccess-Control-Request-Private-Network: true\r\n\
             Connection: close\r\n\r\n"
        );
        let (s, head, _) = raw_tcp(p, &pna).await;
        assert_eq!(s, 204);
        assert!(
            head.to_lowercase()
                .contains("access-control-allow-private-network: true")
        );
    }
    for origin in [
        "https://evil.example",
        "chrome://settings",
        "chrome://ko-machines",
        "null",
        "chrome://ko-workspace.evil",
    ] {
        let (s, head, _) = raw_tcp(
            p,
            &req(p, &format!("Origin: {origin}\r\n"), "/v1/models", Some(&t)),
        )
        .await;
        assert_eq!(s, 403, "{origin}");
        assert!(!head.to_lowercase().contains("access-control-allow-origin"));
        let pre = format!(
            "OPTIONS /v1/models HTTP/1.1\r\nHost: 127.0.0.1:{p}\r\nOrigin: {origin}\r\nAccess-Control-Request-Method: GET\r\nConnection: close\r\n\r\n"
        );
        assert_eq!(raw_tcp(p, &pre).await.0, 403);
    }
    // Cross-site no-cors request (e.g. <img>) without Origin, even for health.
    let (s, _, _) = raw_tcp(
        p,
        &req(p, "Sec-Fetch-Site: cross-site\r\n", "/v1/health", None),
    )
    .await;
    assert_eq!(s, 403);
    let (s, _, _) = raw_tcp(p, &req(p, "Sec-Fetch-Site: none\r\n", "/v1/health", None)).await;
    assert_eq!(s, 200);
    env.stop().await;
}

#[tokio::test]
async fn unix_socket_requires_token() {
    let mut env = TestEnv::start("http://127.0.0.1:9", "").await;
    let sock = env.paths.socket();
    let (s, _, _) = raw_unix(
        &sock,
        "GET /v1/models HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
    )
    .await;
    assert_eq!(s, 401);
    let r = format!(
        "GET /v1/models HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer {}\r\nConnection: close\r\n\r\n",
        env.token
    );
    let (s, _, body) = raw_unix(&sock, &r).await;
    assert_eq!(s, 200);
    assert!(body.contains("claude-code/sonnet"));
    let r = format!(
        "GET /v1/models HTTP/1.1\r\nHost: localhost\r\nOrigin: https://evil.example\r\nAuthorization: Bearer {}\r\nConnection: close\r\n\r\n",
        env.token
    );
    assert_eq!(raw_unix(&sock, &r).await.0, 403);
    env.stop().await;
    assert!(!sock.exists(), "socket removed on shutdown");
    assert!(!env.paths.port_file().exists());
}

#[tokio::test]
async fn request_limits_and_errors_are_json() {
    let mut env = TestEnv::start(
        "http://127.0.0.1:9",
        "[server]\nmax_body_bytes = 1024\nmax_artifact_bytes = 2048\n",
    )
    .await;
    let big = "x".repeat(4096);
    let (s, v) = env.post("/v1/workspaces", json!({"name": big})).await;
    assert_eq!(s, 413, "{v}");
    let (s, v) = env.post("/v1/workspaces", json!({"nope": 1})).await;
    assert_eq!(s, 422);
    assert_eq!(v["error"]["type"], "invalid_request_error");
    let (s, v) = env.get("/v1/does-not-exist").await;
    assert_eq!(s, 404);
    assert!(v["error"].is_object());
    let (s, _) = env.get("/v1/workspaces/..%2F..%2Fetc").await;
    assert_eq!(s, 404);
    let ws = env.create_workspace("limits").await;
    let (s, _) = env
        .post(
            &format!("/v1/workspaces/{ws}/artifacts"),
            json!({"name": "a", "content_text": "y".repeat(2049)}),
        )
        .await;
    assert_eq!(s, 413);
    env.stop().await;
}

#[tokio::test]
async fn second_daemon_on_same_data_dir_is_refused() {
    let mut env = TestEnv::start("http://127.0.0.1:9", "").await;
    let ws = env.create_workspace("lock").await;
    let (s, run) = env
        .post(
            &format!("/v1/workspaces/{ws}/runs"),
            json!({"model": "claude-code/sonnet", "prompt": "SLEEP"}),
        )
        .await;
    assert_eq!(s, 202, "{run}");
    let rid = run["id"].as_str().unwrap().to_owned();
    let mut running = false;
    for _ in 0..200 {
        if env.get(&format!("/v1/runs/{rid}")).await.1["status"] == "running" {
            running = true;
            break;
        }
        tokio::time::sleep(std::time::Duration::from_millis(50)).await;
    }
    assert!(running);
    let tmp_entries = std::fs::read_dir(env.paths.tmp_dir()).unwrap().count();
    let second = ko_agentd::server::start(
        env.paths.clone(),
        ko_agentd::server::ServeOptions {
            port: Some(0),
            unix_socket: Some(true),
        },
    )
    .await;
    let err = format!("{:#}", second.err().expect("second daemon must not start"));
    assert!(err.contains("already running"), "{err}");
    // The first daemon's state is untouched.
    assert_eq!(
        env.get(&format!("/v1/runs/{rid}")).await.1["status"],
        "running"
    );
    assert_eq!(
        std::fs::read_dir(env.paths.tmp_dir()).unwrap().count(),
        tmp_entries
    );
    assert_eq!(env.get("/v1/models").await.0, 200);
    let (s, _) = env
        .send(
            reqwest::Method::POST,
            &format!("/v1/runs/{rid}/cancel"),
            None,
        )
        .await;
    assert_eq!(s, 202);
    env.stop().await;
    // The lock is released on shutdown.
    let again = ko_agentd::server::start(env.paths.clone(), Default::default()).await;
    let again = again.unwrap_or_else(|e| panic!("restart after shutdown: {e:#}"));
    again.shutdown();
    again.wait().await.unwrap();
}
