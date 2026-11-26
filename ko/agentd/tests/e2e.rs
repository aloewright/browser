// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

//! Runs the real `ko-agentd` binary with fake `claude`/`codex` on PATH.

mod common;

use std::io::Write;
use std::os::unix::fs::PermissionsExt;
use std::process::{Command, Stdio};
use std::time::Duration;

use common::*;
use serde_json::{Value, json};

const BIN: &str = env!("CARGO_BIN_EXE_ko-agentd");

fn cli(data: &std::path::Path, args: &[&str], stdin: Option<&str>) -> (i32, String, String) {
    cli_env(data, args, stdin, None)
}

fn cli_env(
    data: &std::path::Path,
    args: &[&str],
    stdin: Option<&str>,
    path: Option<&str>,
) -> (i32, String, String) {
    let mut cmd = Command::new(BIN);
    if let Some(p) = path {
        cmd.env("PATH", p);
    }
    let mut c = cmd
        .arg("--data-dir")
        .arg(data)
        .args(args)
        .stdin(if stdin.is_some() {
            Stdio::piped()
        } else {
            Stdio::null()
        })
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    if let Some(s) = stdin {
        c.stdin.take().unwrap().write_all(s.as_bytes()).unwrap();
    }
    let o = c.wait_with_output().unwrap();
    (
        o.status.code().unwrap_or(-1),
        String::from_utf8_lossy(&o.stdout).into(),
        String::from_utf8_lossy(&o.stderr).into(),
    )
}

#[tokio::test]
async fn binary_serves_with_fake_clis_on_path() {
    let dir = tempfile::tempdir().unwrap();
    let data = dir.path().join("k-o");
    let fakes = dir.path().join("bin");
    install_fakes(&fakes);
    std::fs::create_dir_all(&data).unwrap();
    std::fs::set_permissions(&data, std::fs::Permissions::from_mode(0o700)).unwrap();
    let cfg = data.join("agentd.toml");
    std::fs::write(
        &cfg,
        "[secrets]\nbackend = \"file\"\n\n[providers.claude-code]\nmodels = [\"sonnet\"]\n\n\
         [providers.codex]\nmodels = [\"gpt-5-codex\"]\n\n[providers.nanogpt]\nmodels = [\"m\"]\n",
    )
    .unwrap();
    std::fs::set_permissions(&cfg, std::fs::Permissions::from_mode(0o600)).unwrap();

    // secrets set reads the value from stdin; status/doctor never print it.
    let (code, _, err) = cli(
        &data,
        &["secrets", "set", "nanogpt-api-key"],
        Some("sk-e2e-secret-value\n"),
    );
    assert_eq!(code, 0, "{err}");
    let (code, out, _) = cli(&data, &["secrets", "status"], None);
    assert_eq!(code, 0);
    assert!(out.contains("nanogpt-api-key: set"));
    assert!(!out.contains("sk-e2e-secret-value"));
    let stored = std::fs::read_to_string(data.join("agentd.secrets.json")).unwrap();
    assert!(
        stored.contains("\"sk-e2e-secret-value\""),
        "trailing newline stripped"
    );
    let (code, _, _) = cli(&data, &["secrets", "set", "Bad Name"], Some("x"));
    assert_ne!(code, 0);

    let path = format!(
        "{}:{}",
        fakes.display(),
        std::env::var("PATH").unwrap_or_default()
    );
    let mut child = tokio::process::Command::new(BIN)
        .arg("--data-dir")
        .arg(&data)
        .args(["--log-format", "json", "serve", "--port", "0"])
        .env("PATH", &path)
        .env("ANTHROPIC_API_KEY", "sk-ant-must-not-reach-cli")
        .stdout(Stdio::null())
        .stderr(Stdio::piped())
        .kill_on_drop(true)
        .spawn()
        .unwrap();
    let port_file = data.join("agentd.port");
    assert!(
        eventually(|| port_file.exists()).await,
        "daemon did not start"
    );
    let port: u16 = std::fs::read_to_string(&port_file)
        .unwrap()
        .trim()
        .parse()
        .unwrap();
    let token = std::fs::read_to_string(data.join("agentd.token"))
        .unwrap()
        .trim()
        .to_owned();
    let (code, out, _) = cli(&data, &["token", "show"], None);
    assert_eq!(code, 0);
    assert_eq!(out.trim(), token);

    let client = reqwest::Client::builder().no_proxy().build().unwrap();
    let base = format!("http://127.0.0.1:{port}");
    let h: Value = client
        .get(format!("{base}/v1/health"))
        .send()
        .await
        .unwrap()
        .json()
        .await
        .unwrap();
    assert_eq!(h["status"], "ok");

    // claude resolved via PATH, streamed.
    let r = client
        .post(format!("{base}/v1/chat/completions"))
        .bearer_auth(&token)
        .json(&json!({"model": "claude-code/sonnet", "stream": true, "messages": [{"role": "user", "content": "hi"}]}))
        .send()
        .await
        .unwrap();
    assert_eq!(r.status(), 200);
    let ev = parse_sse(&r.text().await.unwrap());
    assert_eq!(ev.last().unwrap().1, "[DONE]");
    let envs = std::fs::read_to_string(fakes.join("claude.env")).unwrap();
    assert!(
        !envs.contains("ANTHROPIC_API_KEY"),
        "API key must not reach the CLI"
    );
    assert!(envs.contains(&format!("PATH={path}")));

    // codex resolved via PATH.
    let r: Value = client
        .post(format!("{base}/v1/chat/completions"))
        .bearer_auth(&token)
        .json(
            &json!({"model": "codex/gpt-5-codex", "messages": [{"role": "user", "content": "hi"}]}),
        )
        .send()
        .await
        .unwrap()
        .json()
        .await
        .unwrap();
    assert_eq!(
        r["choices"][0]["message"]["content"],
        "Repo contains docs, sdk, and examples directories."
    );

    let (code, out, _) = cli_env(&data, &["doctor"], None, Some(&path));
    assert_eq!(code, 0, "{out}");
    assert!(
        out.contains("claude-code: 9.9.9 (Claude Code fake)"),
        "{out}"
    );
    assert!(out.contains("codex: codex-cli 9.9.9"), "{out}");
    assert!(out.contains("secret nanogpt-api-key is set"));
    assert!(out.contains("agentd.sock (0600)"));
    assert!(!out.contains("sk-e2e-secret-value"));

    // SIGTERM: graceful exit, socket and port file removed.
    // SAFETY: plain kill(2) on our child.
    unsafe {
        libc::kill(child.id().unwrap() as i32, libc::SIGTERM);
    }
    let status = tokio::time::timeout(Duration::from_secs(20), child.wait())
        .await
        .unwrap()
        .unwrap();
    assert!(status.success(), "{status:?}");
    let mut logs = String::new();
    tokio::io::AsyncReadExt::read_to_string(&mut child.stderr.take().unwrap(), &mut logs)
        .await
        .unwrap();
    assert!(logs.contains("ko-agentd listening"));
    for secret in [
        "sk-e2e-secret-value",
        token.as_str(),
        "sk-ant-must-not-reach-cli",
    ] {
        assert!(!logs.contains(secret), "secret in logs");
    }
    for line in logs.lines() {
        serde_json::from_str::<Value>(line).expect("structured JSON log line");
    }
    assert!(!data.join("agentd.sock").exists());
    assert!(!port_file.exists());
}

#[test]
fn refuses_loose_token_permissions() {
    let dir = tempfile::tempdir().unwrap();
    let data = dir.path().join("k-o");
    let (code, out, _) = cli(&data, &["token", "show"], None);
    assert_eq!(code, 0);
    let tok = out.trim().to_owned();
    assert!(tok.len() >= 43);
    std::fs::set_permissions(
        data.join("agentd.token"),
        std::fs::Permissions::from_mode(0o644),
    )
    .unwrap();
    let (code, _, err) = cli(&data, &["token", "show"], None);
    assert_ne!(code, 0);
    assert!(err.contains("0600") || err.contains("rotate"), "{err}");
    let (code, out, _) = cli(&data, &["token", "rotate"], None);
    assert_eq!(code, 0);
    assert_ne!(out.trim(), tok);
    let mode = std::fs::metadata(data.join("agentd.token"))
        .unwrap()
        .permissions()
        .mode()
        & 0o777;
    assert_eq!(mode, 0o600);
}
