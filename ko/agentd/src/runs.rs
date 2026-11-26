// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

//! Agent runs. Each run's events are appended to `<data>/runs/<id>.log` as
//! length-prefixed AES-256-GCM records (AAD binds run id + sequence number,
//! so records cannot be reordered, swapped or edited) and broadcast live.

use std::collections::HashMap;
use std::fs::OpenOptions;
use std::io::Write;
use std::os::unix::fs::OpenOptionsExt;
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use anyhow::{Context, Result, bail};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use tokio::sync::{Semaphore, broadcast};
use tokio_util::sync::CancellationToken;
use zeroize::Zeroizing;

use crate::config::McpServerDef;
use crate::crypto::{StorageKey, run_event_aad};
use crate::db::{self, Run, Workspace};
use crate::error::{ApiError, ApiResult};
use crate::providers::{self, GenEvent, GenParams, ModelRef};
use crate::server::Core;
use crate::util::now_ms;

const MAX_LOG_BYTES: u64 = 256 * 1024 * 1024;

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct RunEvent {
    pub seq: u64,
    pub ts: i64,
    #[serde(rename = "type")]
    pub kind: String,
    pub data: Value,
}

impl RunEvent {
    pub fn is_done(&self) -> bool {
        self.kind == "done"
    }
}

pub fn log_path(core: &Core, run_id: &str) -> PathBuf {
    core.paths.runs_dir().join(format!("{run_id}.log"))
}

struct RunLog {
    file: std::fs::File,
    bytes: u64,
}

impl RunLog {
    fn create(path: &PathBuf) -> Result<Self> {
        let file = OpenOptions::new()
            .append(true)
            .create_new(true)
            .mode(0o600)
            .open(path)
            .with_context(|| format!("creating {}", path.display()))?;
        Ok(Self { file, bytes: 0 })
    }

    fn append(&mut self, key: &StorageKey, run_id: &str, ev: &RunEvent) -> Result<()> {
        let pt = Zeroizing::new(serde_json::to_vec(ev)?);
        let sealed = key.seal(&run_event_aad(run_id, ev.seq), &pt)?;
        let mut rec = Vec::with_capacity(4 + sealed.len());
        rec.extend_from_slice(&(sealed.len() as u32).to_be_bytes());
        rec.extend_from_slice(&sealed);
        self.file.write_all(&rec)?;
        self.bytes += rec.len() as u64;
        Ok(())
    }
}

/// Decrypts a run log. Fails on any tampering; a torn final record (crash
/// mid-write) is ignored.
pub fn read_log(key: &StorageKey, run_id: &str, path: &PathBuf) -> Result<Vec<RunEvent>> {
    let data = match std::fs::read(path) {
        Ok(d) => d,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(vec![]),
        Err(e) => return Err(e).context("reading run log"),
    };
    let mut out = Vec::new();
    let mut pos = 0usize;
    let mut expected = 1u64;
    while pos + 4 <= data.len() {
        let len = u32::from_be_bytes(data[pos..pos + 4].try_into()?) as usize;
        if pos + 4 + len > data.len() {
            tracing::warn!(run = %run_id, "run log ends with a partial record");
            break;
        }
        let pt = key.open(
            &run_event_aad(run_id, expected),
            &data[pos + 4..pos + 4 + len],
        )?;
        let ev: RunEvent = serde_json::from_slice(&pt).context("run log record is not valid")?;
        if ev.seq != expected {
            bail!("run log sequence mismatch");
        }
        out.push(ev);
        expected += 1;
        pos += 4 + len;
    }
    Ok(out)
}

pub fn output_text(events: &[RunEvent]) -> String {
    events
        .iter()
        .filter(|e| e.kind == "text")
        .filter_map(|e| e.data["text"].as_str())
        .collect()
}

struct Active {
    workspace_id: String,
    tx: broadcast::Sender<RunEvent>,
    cancel: CancellationToken,
}

pub struct StartRun {
    pub model: ModelRef,
    pub prompt: Zeroizing<String>,
    pub mcp: Vec<(String, McpServerDef)>,
}

pub struct RunManager {
    core: Arc<Core>,
    sem: Arc<Semaphore>,
    active: Mutex<HashMap<String, Active>>,
    idle: tokio::sync::Notify,
}

struct Recorder {
    core: Arc<Core>,
    run_id: String,
    log: RunLog,
    seq: u64,
    tx: broadcast::Sender<RunEvent>,
    truncated: bool,
    failed: bool,
}

impl Recorder {
    fn emit(&mut self, kind: &str, data: Value) {
        let terminal = kind == "done";
        let cap = self.core.cfg.runs.max_events;
        if !terminal && (self.truncated || self.seq + 2 > cap || self.log.bytes > MAX_LOG_BYTES) {
            if !self.truncated {
                self.truncated = true;
                self.push(
                    "warning",
                    json!({"message": "event log limit reached; further events dropped"}),
                );
            }
            return;
        }
        self.push(kind, data);
    }

    fn push(&mut self, kind: &str, data: Value) {
        self.seq += 1;
        let ev = RunEvent {
            seq: self.seq,
            ts: now_ms(),
            kind: kind.to_owned(),
            data,
        };
        if !self.failed
            && let Err(e) = self.log.append(&self.core.key, &self.run_id, &ev)
        {
            self.failed = true;
            tracing::error!(run = %self.run_id, error = %crate::telemetry::scrub(&format!("{e:#}")), "run log write failed");
        }
        let _ = self.tx.send(ev);
    }
}

impl RunManager {
    pub fn new(core: Arc<Core>) -> Arc<Self> {
        Arc::new(Self {
            sem: Arc::new(Semaphore::new(core.cfg.runs.max_concurrent)),
            core,
            active: Mutex::new(HashMap::new()),
            idle: tokio::sync::Notify::new(),
        })
    }

    fn active(&self) -> std::sync::MutexGuard<'_, HashMap<String, Active>> {
        self.active.lock().unwrap_or_else(|e| e.into_inner())
    }

    pub async fn start(self: &Arc<Self>, ws: &Workspace, req: StartRun) -> ApiResult<Run> {
        if self.core.shutdown.is_cancelled() {
            return Err(ApiError::unavailable("shutting down"));
        }
        let run = Run {
            id: crate::util::new_id(),
            workspace_id: ws.id.clone(),
            model: req.model.id(),
            mcp_servers: req.mcp.iter().map(|(n, _)| n.clone()).collect(),
            status: "queued".into(),
            error: None,
            usage: None,
            created_at: now_ms(),
            started_at: None,
            finished_at: None,
        };
        let log = RunLog::create(&log_path(&self.core, &run.id)).map_err(ApiError::internal)?;
        let r = run.clone();
        self.core.db.call(move |c| db::insert_run(c, &r)).await?;
        let (tx, _) = broadcast::channel(256);
        let cancel = self.core.shutdown.child_token();
        self.active().insert(
            run.id.clone(),
            Active {
                workspace_id: ws.id.clone(),
                tx: tx.clone(),
                cancel: cancel.clone(),
            },
        );
        let mut rec = Recorder {
            core: self.core.clone(),
            run_id: run.id.clone(),
            log,
            seq: 0,
            tx,
            truncated: false,
            failed: false,
        };
        rec.emit("status", json!({"status": "queued"}));
        let this = self.clone();
        let ws = ws.clone();
        let run_id = run.id.clone();
        tokio::spawn(async move {
            this.execute(rec, ws, req, cancel).await;
            this.active().remove(&run_id);
            this.idle.notify_waiters();
        });
        Ok(run)
    }

    async fn execute(
        &self,
        mut rec: Recorder,
        ws: Workspace,
        req: StartRun,
        cancel: CancellationToken,
    ) {
        let started = now_ms();
        let (status, error, usage) = self.drive(&mut rec, &ws, &req, &cancel).await;
        let message = error.as_deref().map(crate::telemetry::scrub);
        rec.emit(
            "done",
            json!({"status": status, "error": message, "usage": usage}),
        );
        let _ = rec.log.file.sync_all();
        let run_id = rec.run_id.clone();
        let (s, m, u) = (status.to_owned(), message.clone(), usage.clone());
        if let Err(e) = self
            .core
            .db
            .call(move |c| db::set_run_finished(c, &run_id, &s, m.as_deref(), u.as_ref(), now_ms()))
            .await
        {
            tracing::error!(error = %crate::telemetry::scrub(&format!("{e:#}")), "recording run result");
        }
        if status == "failed"
            && let Some(m) = &message
        {
            self.core.telemetry.report_error("run_failed", m);
        }
        self.core.telemetry.capture(
            "run_finished",
            json!({
                "provider": req.model.provider(),
                "status": status,
                "mcp_servers": req.mcp.len(),
                "duration_bucket": duration_bucket(now_ms() - started),
            }),
        );
    }

    async fn drive(
        &self,
        rec: &mut Recorder,
        ws: &Workspace,
        req: &StartRun,
        cancel: &CancellationToken,
    ) -> (&'static str, Option<String>, Option<Value>) {
        let _permit = tokio::select! {
            p = self.sem.clone().acquire_owned() => match p {
                Ok(p) => p,
                Err(_) => return ("failed", Some("run queue closed".into()), None),
            },
            _ = cancel.cancelled() => return ("cancelled", None, None),
        };
        let run_id = rec.run_id.clone();
        let _ = self
            .core
            .db
            .call(move |c| db::set_run_started(c, &run_id, now_ms()))
            .await;
        rec.emit("status", json!({"status": "running"}));

        let cwd = match workspace_workdir(&self.core, &ws.id) {
            Ok(p) => p,
            Err(e) => return ("failed", Some(format!("{e:#}")), None),
        };
        let generation = providers::generate(
            &self.core,
            GenParams {
                model: &req.model,
                prompt: req.prompt.clone(),
                mcp: &req.mcp,
                cwd: Some(cwd),
                agent_run: true,
                timeout: Some(Duration::from_secs(self.core.cfg.runs.timeout_secs)),
                cancel: cancel.clone(),
            },
        )
        .await;
        let mut generation = match generation {
            Ok(g) => g,
            Err(e) => return ("failed", Some(e.message), None),
        };
        let mut usage = None;
        while let Some(ev) = generation.events.recv().await {
            match ev {
                GenEvent::Text { text } => rec.emit("text", json!({"text": text})),
                GenEvent::ToolUse { id, name, input } => {
                    rec.emit("tool_use", json!({"id": id, "name": name, "input": input}))
                }
                GenEvent::ToolResult {
                    tool_use_id,
                    is_error,
                    content,
                } => rec.emit(
                    "tool_result",
                    json!({"tool_use_id": tool_use_id, "is_error": is_error, "content": content}),
                ),
                GenEvent::Usage {
                    prompt_tokens,
                    completion_tokens,
                } => {
                    let u = json!({"prompt_tokens": prompt_tokens, "completion_tokens": completion_tokens});
                    rec.emit("usage", u.clone());
                    usage = Some(u);
                }
                GenEvent::Done { .. } => return ("succeeded", None, usage),
                GenEvent::Error { message } => {
                    if cancel.is_cancelled() {
                        return ("cancelled", None, usage);
                    }
                    rec.emit(
                        "error",
                        json!({"message": crate::telemetry::scrub(&message)}),
                    );
                    return ("failed", Some(message), usage);
                }
            }
        }
        if cancel.is_cancelled() {
            ("cancelled", None, usage)
        } else {
            (
                "failed",
                Some("provider stream ended unexpectedly".into()),
                usage,
            )
        }
    }

    pub fn subscribe(&self, run_id: &str) -> Option<broadcast::Receiver<RunEvent>> {
        self.active().get(run_id).map(|a| a.tx.subscribe())
    }

    pub fn is_active(&self, run_id: &str) -> bool {
        self.active().contains_key(run_id)
    }

    pub fn cancel(&self, run_id: &str) -> bool {
        match self.active().get(run_id) {
            Some(a) => {
                a.cancel.cancel();
                true
            }
            None => false,
        }
    }

    pub fn cancel_workspace(&self, workspace_id: &str) {
        for a in self
            .active()
            .values()
            .filter(|a| a.workspace_id == workspace_id)
        {
            a.cancel.cancel();
        }
    }

    pub async fn read_events(&self, run_id: &str) -> Result<Vec<RunEvent>> {
        let core = self.core.clone();
        let id = run_id.to_owned();
        tokio::task::spawn_blocking(move || read_log(&core.key, &id, &log_path(&core, &id))).await?
    }

    /// Waits for active runs to record their final state (shutdown).
    pub async fn wait_idle(&self, timeout: Duration) {
        let deadline = tokio::time::Instant::now() + timeout;
        loop {
            let notified = self.idle.notified();
            if self.active().is_empty() {
                return;
            }
            if tokio::time::timeout_at(deadline, notified).await.is_err() {
                tracing::warn!("runs still active at shutdown");
                return;
            }
        }
    }
}

fn duration_bucket(ms: i64) -> &'static str {
    if ms < 10_000 {
        "<10s"
    } else if ms < 60_000 {
        "<1m"
    } else if ms < 600_000 {
        "<10m"
    } else {
        ">=10m"
    }
}

/// `<workdir_root or data/workspaces>/<id>/work`, created 0700.
pub fn workspace_workdir(core: &Core, workspace_id: &str) -> Result<PathBuf> {
    let root = core
        .cfg
        .workspaces
        .workdir_root
        .clone()
        .unwrap_or_else(|| core.paths.workspaces_dir());
    let dir = root.join(workspace_id).join("work");
    crate::paths::ensure_private_dir(&dir)?;
    Ok(dir)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn log_roundtrip_and_tamper() {
        let t = tempfile::tempdir().unwrap();
        let path = t.path().join("r.log");
        let key = StorageKey::from_bytes(&[3u8; 32]);
        let mut log = RunLog::create(&path).unwrap();
        for i in 1..=3u64 {
            let ev = RunEvent {
                seq: i,
                ts: 0,
                kind: "text".into(),
                data: json!({"text": format!("t{i}")}),
            };
            log.append(&key, "run", &ev).unwrap();
        }
        let evs = read_log(&key, "run", &path).unwrap();
        assert_eq!(evs.len(), 3);
        assert_eq!(output_text(&evs), "t1t2t3");
        // Plaintext is not on disk.
        let raw = std::fs::read(&path).unwrap();
        assert!(!raw.windows(2).any(|w| w == b"t2"));
        // Wrong run id (AAD) fails.
        assert!(read_log(&key, "other", &path).is_err());
        // A flipped byte fails.
        let mut bad = raw.clone();
        bad[20] ^= 1;
        std::fs::write(&path, &bad).unwrap();
        assert!(read_log(&key, "run", &path).is_err());
        // Torn final record is tolerated.
        let mut torn = raw.clone();
        torn.truncate(raw.len() - 5);
        std::fs::write(&path, &torn).unwrap();
        assert_eq!(read_log(&key, "run", &path).unwrap().len(), 2);
        // Dropping a record breaks the sequence.
        let first_len = u32::from_be_bytes(raw[..4].try_into().unwrap()) as usize + 4;
        std::fs::write(&path, &raw[first_len..]).unwrap();
        assert!(read_log(&key, "run", &path).is_err());
    }
}
