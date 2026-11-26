// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

//! Cloudflare worker deploy and telemetry against mock APIs.

mod common;

use std::time::Duration;

use common::*;
use serde_json::{Value, json};
use wiremock::matchers::{header, method, path};
use wiremock::{Mock, MockServer, Request, ResponseTemplate};

fn ok(result: Value) -> ResponseTemplate {
    ResponseTemplate::new(200)
        .set_body_json(json!({"success": true, "errors": [], "messages": [], "result": result}))
}

async fn mount_cf(mock: &MockServer, existing_tag: Option<&str>) {
    let acct = format!("/client/v4/accounts/{ACCOUNT}/workers");
    let auth = format!("Bearer {CF_API_TOKEN}");
    let scripts = match existing_tag {
        Some(tag) => {
            json!([{"id": "ko-agent-other", "migration_tag": "v1"}, {"id": "__NAME__", "migration_tag": tag}])
        }
        None => json!([{"id": "ko-agent-other", "migration_tag": "v1"}]),
    };
    Mock::given(method("GET"))
        .and(path(format!("{acct}/scripts")))
        .and(header("authorization", auth.as_str()))
        .respond_with(ok(scripts))
        .mount(mock)
        .await;
    Mock::given(method("PUT"))
        .and(header("authorization", auth.as_str()))
        .respond_with(ok(json!({"id": "x"})))
        .mount(mock)
        .await;
    Mock::given(method("POST"))
        .and(header("authorization", auth.as_str()))
        .respond_with(ok(json!({"enabled": true})))
        .mount(mock)
        .await;
    Mock::given(method("GET"))
        .and(path(format!("{acct}/subdomain")))
        .and(header("authorization", auth.as_str()))
        .respond_with(ok(json!({"subdomain": "example-sub"})))
        .mount(mock)
        .await;
}

fn multipart_parts(req: &Request) -> (Value, String) {
    let ct = req
        .headers
        .get("content-type")
        .unwrap()
        .to_str()
        .unwrap()
        .to_owned();
    let boundary = ct
        .split("boundary=")
        .nth(1)
        .unwrap()
        .trim_matches('"')
        .to_owned();
    let body = String::from_utf8_lossy(&req.body).to_string();
    let mut metadata = Value::Null;
    let mut module = String::new();
    for part in body.split(&format!("--{boundary}")) {
        let Some((head, content)) = part.split_once("\r\n\r\n") else {
            continue;
        };
        let content = content.trim_end_matches("\r\n");
        if head.contains("name=\"metadata\"") {
            assert!(head.contains("application/json"));
            metadata = serde_json::from_str(content).unwrap();
        } else if head.contains("name=\"index.js\"") {
            assert!(head.contains("application/javascript+module"));
            assert!(head.contains("filename=\"index.js\""));
            module = content.to_owned();
        }
    }
    (metadata, module)
}

#[tokio::test]
async fn deploys_workspace_agent() {
    let mock = MockServer::start().await;
    mount_cf(&mock, None).await;
    let mut env = TestEnv::start(&mock.uri(), "").await;
    let ws = env.create_workspace("cloud").await;
    let (s, v) = env
        .post("/v1/cloudflare/agents", json!({"workspace_id": ws}))
        .await;
    assert_eq!(s, 201, "{v}");
    let name = format!("ko-agent-{}", ws.replace('-', ""));
    assert_eq!(v["script_name"], name.as_str());
    assert_eq!(v["url"], format!("https://{name}.example-sub.workers.dev"));

    let reqs = mock.received_requests().await.unwrap();
    let put = reqs.iter().find(|r| r.method.as_str() == "PUT").unwrap();
    assert_eq!(
        put.url.path(),
        format!("/client/v4/accounts/{ACCOUNT}/workers/scripts/{name}")
    );
    let (meta, module) = multipart_parts(put);
    assert!(module.contains("export class KoAgent"));
    assert_eq!(meta["main_module"], "index.js");
    assert_eq!(meta["compatibility_flags"], json!(["nodejs_compat"]));
    assert_eq!(
        meta["migrations"],
        json!({"new_tag": "v1", "new_sqlite_classes": ["KoAgent"]})
    );
    let bindings = meta["bindings"].as_array().unwrap();
    let find = |n: &str| bindings.iter().find(|b| b["name"] == n).cloned().unwrap();
    assert_eq!(find("KoAgent")["type"], "durable_object_namespace");
    assert_eq!(find("KoAgent")["class_name"], "KoAgent");
    assert_eq!(
        find("ACCESS_TEAM_DOMAIN")["text"],
        "example.cloudflareaccess.com"
    );
    assert_eq!(find("ACCESS_AUD")["text"], "aud0123456789");
    assert_eq!(find("KO_WORKSPACE_ID")["text"], ws.as_str());
    assert_eq!(find("AI_GATEWAY_TOKEN")["type"], "secret_text");
    assert_eq!(find("AI_GATEWAY_TOKEN")["text"], CF_AIG);
    let sub = reqs.iter().find(|r| r.method.as_str() == "POST").unwrap();
    assert_eq!(
        sub.url.path(),
        format!("/client/v4/accounts/{ACCOUNT}/workers/scripts/{name}/subdomain")
    );

    let (_, w) = env.get(&format!("/v1/workspaces/{ws}")).await;
    assert_eq!(w["cloudflare_agent"]["script_name"], name.as_str());
    let (_, a) = env.get(&format!("/v1/workspaces/{ws}/agents")).await;
    let cf = a["data"]
        .as_array()
        .unwrap()
        .iter()
        .find(|x| x["kind"] == "cloudflare")
        .cloned()
        .unwrap();
    assert_eq!(cf["url"], v["url"]);
    env.stop().await;
}

#[tokio::test]
async fn redeploy_skips_applied_migration() {
    let mock = MockServer::start().await;
    let mut env = TestEnv::start(&mock.uri(), "").await;
    let ws = env.create_workspace("cloud").await;
    let name = format!("ko-agent-{}", ws.replace('-', ""));
    let acct = format!("/client/v4/accounts/{ACCOUNT}/workers");
    Mock::given(method("GET"))
        .and(path(format!("{acct}/scripts")))
        .respond_with(ok(json!([{"id": name, "migration_tag": "v1"}])))
        .mount(&mock)
        .await;
    mount_cf(&mock, Some("v1")).await;
    let (s, v) = env
        .post("/v1/cloudflare/agents", json!({"workspace_id": ws}))
        .await;
    assert_eq!(s, 201, "{v}");
    let reqs = mock.received_requests().await.unwrap();
    let put = reqs.iter().find(|r| r.method.as_str() == "PUT").unwrap();
    let (meta, _) = multipart_parts(put);
    assert!(meta.get("migrations").is_none(), "{meta}");
    env.stop().await;
}

#[tokio::test]
async fn deploy_errors_are_scrubbed_and_config_required() {
    let mock = MockServer::start().await;
    Mock::given(method("GET")).respond_with(
        ResponseTemplate::new(403).set_body_json(json!({"success": false, "errors": [{"code": 10000, "message": format!("Authentication error for token {CF_API_TOKEN}")}]})),
    ).mount(&mock).await;
    let mut env = TestEnv::start(&mock.uri(), "").await;
    let ws = env.create_workspace("cloud").await;
    let (s, v) = env
        .post("/v1/cloudflare/agents", json!({"workspace_id": ws}))
        .await;
    assert_eq!(s, 502, "{v}");
    assert!(v.to_string().contains("10000"));
    assert!(!v.to_string().contains(CF_API_TOKEN), "{v}");
    let (s, _) = env
        .post("/v1/cloudflare/agents", json!({"workspace_id": "nope"}))
        .await;
    assert_eq!(s, 404);
    env.stop().await;

    // Access settings have no defaults.
    let mock = MockServer::start().await;
    let mut env = TestEnv::start(&mock.uri(), "").await;
    let cfg = std::fs::read_to_string(env.paths.config())
        .unwrap()
        .replace("access_aud = \"aud0123456789\"\n", "");
    env.stop().await;
    std::fs::write(env.paths.config(), cfg).unwrap();
    let server = ko_agentd::server::start(
        env.paths.clone(),
        ko_agentd::server::ServeOptions {
            port: Some(0),
            unix_socket: Some(false),
        },
    )
    .await
    .unwrap();
    env.base = format!("http://127.0.0.1:{}", server.tcp_addr.port());
    let ws = env.create_workspace("cloud").await;
    let (s, v) = env
        .post("/v1/cloudflare/agents", json!({"workspace_id": ws}))
        .await;
    assert_eq!(s, 503);
    assert!(
        v["error"]["message"]
            .as_str()
            .unwrap()
            .contains("access_aud")
    );
    assert!(mock.received_requests().await.unwrap().is_empty());
    server.shutdown();
    server.wait().await.unwrap();
}

#[tokio::test]
async fn telemetry_sends_scrubbed_events_when_configured() {
    let mock = MockServer::start().await;
    Mock::given(method("POST"))
        .and(path("/api/7/envelope/"))
        .respond_with(ResponseTemplate::new(200))
        .mount(&mock)
        .await;
    Mock::given(method("POST"))
        .and(path("/i/v0/e/"))
        .respond_with(ResponseTemplate::new(200))
        .mount(&mock)
        .await;
    let host = mock.uri();
    let dsn = host.replace("http://", "http://glitchkey@") + "/7";
    let extra = format!(
        "[telemetry.glitchtip]\ndsn_secret = \"glitchtip-dsn\"\nenvironment = \"test\"\n\n[telemetry.posthog]\nhost = \"{host}\"\nproject_key = \"phc_testkey\"\n"
    );
    // Seed the DSN before start by writing the secret file through a first env.
    let mut env = TestEnv::start(&host, &extra).await;
    env.stop().await;
    let store = ko_agentd::secrets::SecretStore::open(
        ko_agentd::config::SecretBackendKind::File,
        &env.paths,
    )
    .unwrap();
    store.set("glitchtip-dsn", &dsn).unwrap();
    let server = ko_agentd::server::start(
        env.paths.clone(),
        ko_agentd::server::ServeOptions {
            port: Some(0),
            unix_socket: Some(false),
        },
    )
    .await
    .unwrap();
    env.base = format!("http://127.0.0.1:{}", server.tcp_addr.port());

    let ws = env.create_workspace("t").await;
    let (_, run) = env
        .post(
            &format!("/v1/workspaces/{ws}/runs"),
            json!({"model": "claude-code/sonnet", "prompt": "FAIL with PRIVATE-PROMPT"}),
        )
        .await;
    let rid = run["id"].as_str().unwrap().to_owned();
    for _ in 0..100 {
        let (_, r) = env.get(&format!("/v1/runs/{rid}")).await;
        if r["status"] == "failed" {
            break;
        }
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
    let mut envelopes = vec![];
    let mut events = vec![];
    for _ in 0..100 {
        let reqs = mock.received_requests().await.unwrap();
        envelopes = reqs
            .iter()
            .filter(|r| r.url.path() == "/api/7/envelope/")
            .cloned()
            .collect();
        events = reqs
            .iter()
            .filter(|r| r.url.path() == "/i/v0/e/")
            .cloned()
            .collect();
        if !envelopes.is_empty() && events.len() >= 2 {
            break;
        }
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
    assert!(!envelopes.is_empty(), "no GlitchTip envelope");
    let e = &envelopes[0];
    let auth = e.headers.get("x-sentry-auth").unwrap().to_str().unwrap();
    assert!(auth.contains("sentry_key=glitchkey"));
    let body = String::from_utf8_lossy(&e.body);
    assert!(body.contains("model refused"));
    assert!(!body.contains("PRIVATE-PROMPT"));
    assert!(!body.contains(&env.token));
    let names: Vec<String> = events
        .iter()
        .map(|r| {
            serde_json::from_slice::<Value>(&r.body).unwrap()["event"]
                .as_str()
                .unwrap()
                .to_owned()
        })
        .collect();
    assert!(names.contains(&"agentd_started".to_owned()), "{names:?}");
    for r in &events {
        let b = String::from_utf8_lossy(&r.body);
        assert!(b.contains("phc_testkey"));
        assert!(!b.contains("PRIVATE-PROMPT"));
        assert!(!b.contains(&ws), "workspace ids are not sent");
    }
    server.shutdown();
    server.wait().await.unwrap();
}
