// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#![allow(dead_code)]

use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::time::Duration;

use ko_agentd::config::SecretBackendKind;
use ko_agentd::paths::Paths;
use ko_agentd::secrets::SecretStore;
use ko_agentd::server::{RunningServer, ServeOptions, start};
use serde_json::Value;
use tokio::io::{AsyncReadExt, AsyncWriteExt};

pub const ACCOUNT: &str = "0123456789abcdef0123456789abcdef";
pub const NANO_KEY: &str = "sk-nano-test-key-0001";
pub const CF_AIG: &str = "cf-aig-test-token-0002";
pub const CF_OPENAI_KEY: &str = "sk-openai-test-key-0003";
pub const CF_API_TOKEN: &str = "cf-api-test-token-0004";
pub const MCP_TOKEN: &str = "mcp-docs-test-token-0005";
pub const GH_TOKEN: &str = "ghp_test_token_0006";

/// Copies fake CLIs and fixtures into `dir`.
pub fn install_fakes(dir: &Path) {
    std::fs::create_dir_all(dir).unwrap();
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests");
    for (src, dst) in [
        ("fakes/claude", "claude"),
        ("fakes/codex", "codex"),
        ("fixtures/claude_stream.jsonl", "claude_stream.jsonl"),
        ("fixtures/codex_stream.jsonl", "codex_stream.jsonl"),
    ] {
        std::fs::copy(root.join(src), dir.join(dst)).unwrap();
    }
    for b in ["claude", "codex"] {
        std::fs::set_permissions(dir.join(b), std::fs::Permissions::from_mode(0o755)).unwrap();
    }
}

pub struct Mocks {
    pub upstream: String,
}

/// agentd.toml pointing every remote endpoint at `mock` (loopback).
pub fn config_toml(fakes: &Path, mock: &str, extra: &str) -> String {
    format!(
        r#"
[secrets]
backend = "file"

[network]
allow_loopback_http = true

[providers.claude-code]
binary = "{fakes}/claude"
models = ["sonnet", "opus"]
permission_mode = "acceptEdits"
timeout_secs = 60

[providers.codex]
binary = "{fakes}/codex"
models = ["gpt-5-codex"]
sandbox = "workspace-write"
timeout_secs = 60

[providers.cf-gateway]
account_id = "{ACCOUNT}"
gateway = "ko-gw"
base_url = "{mock}/v1"
models = ["openai/gpt-4o-mini"]

[providers.nanogpt]
base_url = "{mock}/api/v1"
models = ["chatgpt-4o-latest"]

[mcp.servers.github]
type = "stdio"
command = "npx"
args = ["-y", "@modelcontextprotocol/server-github"]
env_secrets = {{ GITHUB_PERSONAL_ACCESS_TOKEN = "mcp-github-token" }}

[cloudflare]
account_id = "{ACCOUNT}"
api_base = "{mock}/client/v4"
access_team_domain = "example.cloudflareaccess.com"
access_aud = "aud0123456789"
ai_gateway_id = "ko-gw"
aig_token_secret = "cf-gateway-aig-token"
bundle_path = "{fakes}/worker.js"

{extra}
"#,
        fakes = fakes.display()
    )
}

pub struct TestEnv {
    pub dir: tempfile::TempDir,
    pub paths: Paths,
    pub fakes: PathBuf,
    pub server: Option<RunningServer>,
    pub token: String,
    pub base: String,
    pub client: reqwest::Client,
}

impl TestEnv {
    pub async fn start(mock: &str, extra: &str) -> Self {
        let dir = tempfile::tempdir().unwrap();
        let paths = Paths::new(dir.path().join("k-o"));
        paths.ensure().unwrap();
        let fakes = dir.path().join("fakes");
        install_fakes(&fakes);
        std::fs::write(
            fakes.join("worker.js"),
            "export default { fetch() { return new Response('ok') } }\nexport class KoAgent {}\n",
        )
        .unwrap();
        let cfg = paths.config();
        std::fs::write(&cfg, config_toml(&fakes, mock, extra)).unwrap();
        std::fs::set_permissions(&cfg, std::fs::Permissions::from_mode(0o600)).unwrap();
        let secrets = SecretStore::open(SecretBackendKind::File, &paths).unwrap();
        for (k, v) in [
            ("nanogpt-api-key", NANO_KEY),
            ("cf-gateway-aig-token", CF_AIG),
            ("cf-gateway-key-openai", CF_OPENAI_KEY),
            ("cloudflare-api-token", CF_API_TOKEN),
            ("mcp-docs-token", MCP_TOKEN),
            ("mcp-github-token", GH_TOKEN),
        ] {
            secrets.set(k, v).unwrap();
        }
        let server = start(
            paths.clone(),
            ServeOptions {
                port: Some(0),
                unix_socket: Some(true),
            },
        )
        .await
        .unwrap();
        let token = std::fs::read_to_string(paths.token())
            .unwrap()
            .trim()
            .to_owned();
        let base = format!("http://127.0.0.1:{}", server.tcp_addr.port());
        let client = reqwest::Client::builder().no_proxy().build().unwrap();
        Self {
            dir,
            paths,
            fakes,
            server: Some(server),
            token,
            base,
            client,
        }
    }

    pub fn port(&self) -> u16 {
        self.server.as_ref().unwrap().tcp_addr.port()
    }

    pub fn url(&self, p: &str) -> String {
        format!("{}{p}", self.base)
    }

    pub async fn get(&self, p: &str) -> (u16, Value) {
        let r = self
            .client
            .get(self.url(p))
            .bearer_auth(&self.token)
            .send()
            .await
            .unwrap();
        let s = r.status().as_u16();
        (s, r.json().await.unwrap_or(Value::Null))
    }

    pub async fn send(
        &self,
        method: reqwest::Method,
        p: &str,
        body: Option<Value>,
    ) -> (u16, Value) {
        let mut rb = self
            .client
            .request(method, self.url(p))
            .bearer_auth(&self.token);
        if let Some(b) = body {
            rb = rb.json(&b);
        }
        let r = rb.send().await.unwrap();
        let s = r.status().as_u16();
        (s, r.json().await.unwrap_or(Value::Null))
    }

    pub async fn post(&self, p: &str, body: Value) -> (u16, Value) {
        self.send(reqwest::Method::POST, p, Some(body)).await
    }

    /// Reads an SSE response into (event, data) pairs.
    pub async fn sse(&self, p: &str, body: Option<Value>) -> Vec<(String, String)> {
        let rb = match body {
            Some(b) => self.client.post(self.url(p)).json(&b),
            None => self.client.get(self.url(p)),
        };
        let r = rb.bearer_auth(&self.token).send().await.unwrap();
        assert_eq!(r.status().as_u16(), 200, "SSE status");
        assert!(
            r.headers()["content-type"]
                .to_str()
                .unwrap()
                .starts_with("text/event-stream")
        );
        let text = tokio::time::timeout(Duration::from_secs(30), r.text())
            .await
            .expect("SSE timed out")
            .unwrap();
        parse_sse(&text)
    }

    pub async fn create_workspace(&self, name: &str) -> String {
        let (s, w) = self
            .post(
                "/v1/workspaces",
                serde_json::json!({"name": name, "container_id": "ctr-1"}),
            )
            .await;
        assert_eq!(s, 201, "{w}");
        w["id"].as_str().unwrap().to_owned()
    }

    pub fn fake_file(&self, name: &str) -> String {
        std::fs::read_to_string(self.fakes.join(name)).unwrap_or_default()
    }

    /// Restarts the daemon with an edited agentd.toml (same data dir).
    pub async fn reconfigure(&mut self, edit: impl FnOnce(String) -> String) {
        self.stop().await;
        let cfg = std::fs::read_to_string(self.paths.config()).unwrap();
        std::fs::write(self.paths.config(), edit(cfg)).unwrap();
        let server = start(
            self.paths.clone(),
            ServeOptions {
                port: Some(0),
                unix_socket: Some(true),
            },
        )
        .await
        .unwrap();
        self.base = format!("http://127.0.0.1:{}", server.tcp_addr.port());
        self.server = Some(server);
    }

    pub async fn stop(&mut self) {
        if let Some(s) = self.server.take() {
            s.shutdown();
            tokio::time::timeout(Duration::from_secs(20), s.wait())
                .await
                .expect("shutdown timed out")
                .unwrap();
        }
    }
}

pub fn parse_sse(text: &str) -> Vec<(String, String)> {
    let mut out = Vec::new();
    for block in text.split("\n\n") {
        let mut event = String::from("message");
        let mut data = Vec::new();
        for line in block.lines() {
            if let Some(v) = line.strip_prefix("event:") {
                event = v.trim().to_owned();
            } else if let Some(v) = line.strip_prefix("data:") {
                data.push(v.strip_prefix(' ').unwrap_or(v).to_owned());
            }
        }
        if !data.is_empty() {
            out.push((event, data.join("\n")));
        }
    }
    out
}

/// Sends a raw HTTP/1.1 request; returns (status, raw head, body).
pub async fn raw_tcp(port: u16, req: &str) -> (u16, String, String) {
    let mut s = tokio::net::TcpStream::connect(("127.0.0.1", port))
        .await
        .unwrap();
    s.write_all(req.as_bytes()).await.unwrap();
    let mut buf = Vec::new();
    tokio::time::timeout(Duration::from_secs(10), s.read_to_end(&mut buf))
        .await
        .unwrap()
        .unwrap();
    split_response(&buf)
}

pub async fn raw_unix(path: &Path, req: &str) -> (u16, String, String) {
    let mut s = tokio::net::UnixStream::connect(path).await.unwrap();
    s.write_all(req.as_bytes()).await.unwrap();
    let mut buf = Vec::new();
    tokio::time::timeout(Duration::from_secs(10), s.read_to_end(&mut buf))
        .await
        .unwrap()
        .unwrap();
    split_response(&buf)
}

fn split_response(buf: &[u8]) -> (u16, String, String) {
    let text = String::from_utf8_lossy(buf).to_string();
    let (head, body) = text.split_once("\r\n\r\n").unwrap_or((&text, ""));
    let status = head
        .split_whitespace()
        .nth(1)
        .and_then(|s| s.parse().ok())
        .unwrap_or(0);
    (status, head.to_owned(), body.to_owned())
}

/// Polls `f` until it returns true (max ~10s).
pub async fn eventually<F: FnMut() -> bool>(mut f: F) -> bool {
    for _ in 0..200 {
        if f() {
            return true;
        }
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
    false
}
