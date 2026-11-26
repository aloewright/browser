// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

//! Runs an official CLI (claude / codex) headless. The prompt goes on
//! stdin; secrets never go on argv. The child gets a minimal environment,
//! its own process group and a private TMPDIR.

use std::path::{Path, PathBuf};
use std::process::Stdio;
use std::time::Duration;

use anyhow::{Context, Result};
use serde_json::Value;
use tokio::io::{AsyncBufRead, AsyncBufReadExt, AsyncRead, AsyncReadExt, AsyncWriteExt, BufReader};
use tokio::sync::mpsc;
use tokio_util::sync::CancellationToken;
use zeroize::Zeroizing;

use super::GenEvent;

/// Max bytes of one stdout JSON line; longer lines are skipped.
pub const MAX_LINE: usize = 8 * 1024 * 1024;
const STDERR_TAIL: usize = 2048;

/// Inherited from the daemon's environment when set.
pub const BASE_ENV: &[&str] = &[
    "HOME",
    "PATH",
    "USER",
    "LOGNAME",
    "SHELL",
    "LANG",
    "LC_ALL",
    "LC_CTYPE",
    "TERM",
    "XDG_CONFIG_HOME",
    "XDG_DATA_HOME",
    "XDG_CACHE_HOME",
    "XDG_STATE_HOME",
    "XDG_RUNTIME_DIR",
    "SSL_CERT_FILE",
    "SSL_CERT_DIR",
    "NODE_EXTRA_CA_CERTS",
    "HTTPS_PROXY",
    "https_proxy",
    "NO_PROXY",
    "no_proxy",
];

pub trait LineMapper: Send + 'static {
    fn map(&mut self, line: &Value) -> Vec<GenEvent>;
}

pub struct CliSpec {
    pub program: String,
    pub args: Vec<String>,
    /// Extra env (may hold secrets for MCP servers; never logged).
    pub env: Vec<(String, Zeroizing<String>)>,
    /// Env var names passed through from the daemon env.
    pub pass_env: Vec<String>,
    pub cwd: PathBuf,
    pub tmpdir: PathBuf,
    pub stdin: Zeroizing<String>,
    pub timeout: Duration,
    pub cancel: CancellationToken,
    /// Removed once the CLI has produced its first line (it has read it).
    pub delete_after_start: Option<PathBuf>,
}

/// Resolves a bare program name against PATH (absolute paths kept).
pub fn which(program: &str) -> Option<PathBuf> {
    let p = Path::new(program);
    if p.is_absolute() {
        return p.is_file().then(|| p.to_path_buf());
    }
    if program.contains('/') {
        return None;
    }
    let path = std::env::var_os("PATH")?;
    std::env::split_paths(&path)
        .map(|d| d.join(program))
        .find(|c| is_executable(c))
}

fn is_executable(p: &Path) -> bool {
    use std::os::unix::fs::PermissionsExt;
    std::fs::metadata(p).is_ok_and(|m| m.is_file() && m.permissions().mode() & 0o111 != 0)
}

/// Reads one '\n'-terminated line into `buf`, keeping at most `max` bytes.
/// Returns (bytes consumed, truncated).
async fn read_line_capped<R: AsyncBufRead + Unpin>(
    r: &mut R,
    buf: &mut Vec<u8>,
    max: usize,
) -> std::io::Result<(usize, bool)> {
    let mut consumed = 0;
    let mut truncated = false;
    loop {
        let avail = r.fill_buf().await?;
        if avail.is_empty() {
            return Ok((consumed, truncated));
        }
        let (chunk, done) = match avail.iter().position(|&b| b == b'\n') {
            Some(i) => (&avail[..=i], true),
            None => (avail, false),
        };
        let take = chunk.len();
        if buf.len() + take <= max {
            buf.extend_from_slice(chunk);
        } else {
            truncated = true;
        }
        r.consume(take);
        consumed += take;
        if done {
            return Ok((consumed, truncated));
        }
    }
}

async fn read_tail<R: AsyncRead + Unpin>(mut r: R) -> String {
    let mut tail: Vec<u8> = Vec::new();
    let mut buf = [0u8; 4096];
    loop {
        match r.read(&mut buf).await {
            Ok(0) | Err(_) => break,
            Ok(n) => {
                tail.extend_from_slice(&buf[..n]);
                if tail.len() > STDERR_TAIL * 2 {
                    let cut = tail.len() - STDERR_TAIL;
                    tail.drain(..cut);
                }
            }
        }
    }
    let s = String::from_utf8_lossy(&tail);
    let s = s.trim();
    let start = s.len().saturating_sub(STDERR_TAIL);
    let mut start = start;
    while !s.is_char_boundary(start) {
        start += 1;
    }
    crate::telemetry::scrub(&s[start..])
}

fn signal_group(pid: Option<u32>, sig: i32) {
    if let Some(pid) = pid {
        // SAFETY: plain syscall; the child leads its own process group.
        unsafe {
            libc::killpg(pid as libc::pid_t, sig);
        }
    }
}

/// Spawns the CLI and streams mapped events. The stream always ends with a
/// terminal event (Done or Error). Dropping the receiver kills the child.
pub fn spawn(spec: CliSpec, mut mapper: Box<dyn LineMapper>) -> Result<mpsc::Receiver<GenEvent>> {
    let program =
        which(&spec.program).with_context(|| format!("{} CLI not found on PATH", spec.program))?;
    let mut cmd = tokio::process::Command::new(&program);
    cmd.args(&spec.args)
        .env_clear()
        .current_dir(&spec.cwd)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .kill_on_drop(true)
        .process_group(0);
    for k in BASE_ENV
        .iter()
        .copied()
        .chain(spec.pass_env.iter().map(String::as_str))
    {
        if let Some(v) = std::env::var_os(k) {
            cmd.env(k, v);
        }
    }
    cmd.env("TMPDIR", &spec.tmpdir);
    for (k, v) in &spec.env {
        cmd.env(k, v.as_str());
    }
    let mut child = cmd
        .spawn()
        .with_context(|| format!("starting {}", spec.program))?;
    let pid = child.id();
    let mut stdin = child.stdin.take().context("no stdin")?;
    let stdout = child.stdout.take().context("no stdout")?;
    let stderr = child.stderr.take().context("no stderr")?;
    let name = spec.program.clone();
    let (tx, rx) = mpsc::channel::<GenEvent>(64);
    let prompt = spec.stdin;
    let cancel = spec.cancel;
    let timeout = spec.timeout;
    let mut delete_after_start = spec.delete_after_start;

    tokio::spawn(async move {
        let writer = tokio::spawn(async move {
            let _ = stdin.write_all(prompt.as_bytes()).await;
            let _ = stdin.shutdown().await;
        });
        let stderr_task = tokio::spawn(read_tail(stderr));
        let mut reader = BufReader::new(stdout);
        let mut buf = Vec::new();
        let deadline = tokio::time::sleep(timeout);
        tokio::pin!(deadline);
        let mut terminal = false;
        let mut abort: Option<String> = None;
        loop {
            buf.clear();
            tokio::select! {
                _ = cancel.cancelled() => { abort = Some("cancelled".into()); break; }
                _ = &mut deadline => { abort = Some(format!("{name} timed out")); break; }
                _ = tx.closed() => { abort = Some("client went away".into()); break; }
                r = read_line_capped(&mut reader, &mut buf, MAX_LINE) => match r {
                    Ok((0, _)) => break,
                    Ok((_, true)) => {
                        tracing::warn!(cli = %name, "skipping oversized output line");
                    }
                    Ok((_, false)) => {
                        if let Some(p) = delete_after_start.take() {
                            let _ = std::fs::remove_file(p);
                        }
                        let Ok(v) = serde_json::from_slice::<Value>(&buf) else { continue };
                        for ev in mapper.map(&v) {
                            terminal |= ev.is_terminal();
                            if tx.send(ev).await.is_err() {
                                abort = Some("client went away".into());
                                break;
                            }
                        }
                        if abort.is_some() { break; }
                    }
                    Err(e) => { abort = Some(format!("reading {name} output: {e}")); break; }
                },
            }
        }
        if abort.is_some() {
            signal_group(pid, libc::SIGTERM);
            if tokio::time::timeout(Duration::from_secs(3), child.wait())
                .await
                .is_err()
            {
                signal_group(pid, libc::SIGKILL);
                let _ = child.kill().await;
            }
        }
        let status = tokio::time::timeout(Duration::from_secs(10), child.wait()).await;
        writer.abort();
        let tail = tokio::time::timeout(Duration::from_secs(2), stderr_task)
            .await
            .ok()
            .and_then(Result::ok)
            .unwrap_or_default();
        if let Some(reason) = abort {
            if !terminal {
                let _ = tx.send(GenEvent::Error { message: reason }).await;
            }
            return;
        }
        if terminal {
            return;
        }
        let message = match status {
            Ok(Ok(s)) if s.success() => format!("{name} exited without a result"),
            Ok(Ok(s)) => {
                let code = s
                    .code()
                    .map_or_else(|| "signal".to_owned(), |c| c.to_string());
                if tail.is_empty() {
                    format!("{name} exited with status {code}")
                } else {
                    format!("{name} exited with status {code}: {tail}")
                }
            }
            _ => format!("{name} did not exit"),
        };
        let _ = tx.send(GenEvent::Error { message }).await;
    });
    Ok(rx)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn capped_lines() {
        let data = b"short\nthis-line-is-long\nend".to_vec();
        let mut r = BufReader::with_capacity(4, &data[..]);
        let mut buf = Vec::new();
        assert_eq!(
            read_line_capped(&mut r, &mut buf, 8).await.unwrap(),
            (6, false)
        );
        assert_eq!(buf, b"short\n");
        buf.clear();
        let (n, trunc) = read_line_capped(&mut r, &mut buf, 8).await.unwrap();
        assert!(trunc && n == 18);
        buf.clear();
        assert_eq!(
            read_line_capped(&mut r, &mut buf, 8).await.unwrap(),
            (3, false)
        );
        assert_eq!(buf, b"end");
    }
}
