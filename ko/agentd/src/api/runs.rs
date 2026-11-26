// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

use std::convert::Infallible;
use std::time::Duration;

use axum::Json;
use axum::extract::{Path, State};
use axum::http::{HeaderMap, StatusCode};
use axum::response::IntoResponse;
use axum::response::sse::{Event, KeepAlive, Sse};
use futures::Stream;
use serde::Deserialize;
use serde_json::Value;
use tokio::sync::broadcast::error::RecvError;
use zeroize::Zeroizing;

use super::{ApiJson, data, id_or_404, load_workspace};
use crate::db::{self, Run};
use crate::error::{ApiError, ApiResult};
use crate::runs::{RunEvent, StartRun};
use crate::server::AppState;

const MAX_PROMPT: usize = 1024 * 1024;

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct StartBody {
    /// "claude-code" | "codex" | "cf-gateway" | "nanogpt"; with `model`.
    provider: Option<String>,
    /// Full model id, or the bare model when `provider` is given.
    model: Option<String>,
    prompt: String,
    /// Defaults to the workspace's MCP servers; [] for none.
    mcp_servers: Option<Vec<String>>,
}

async fn load_run(st: &AppState, rid: &str) -> ApiResult<Run> {
    let rid = id_or_404(rid, "run")?;
    st.core
        .db
        .call(move |c| db::get_run(c, &rid))
        .await?
        .ok_or_else(|| ApiError::not_found("run"))
}

pub async fn start(
    State(st): State<AppState>,
    Path(id): Path<String>,
    ApiJson(b): ApiJson<StartBody>,
) -> ApiResult<impl IntoResponse> {
    let ws = load_workspace(&st, &id).await?;
    if b.prompt.trim().is_empty() || b.prompt.len() > MAX_PROMPT {
        return Err(ApiError::bad_request("prompt must be 1 byte to 1 MiB"));
    }
    let model_id = match (b.provider, b.model) {
        (Some(p), Some(m)) => format!("{p}/{m}"),
        (Some(_), None) => return Err(ApiError::bad_request("model is required with provider")),
        (None, Some(m)) => m,
        (None, None) => ws
            .default_model
            .clone()
            .or_else(|| st.core.cfg.workspaces.default_model.clone())
            .ok_or_else(|| ApiError::bad_request("model is required (no workspace default)"))?,
    };
    let model = crate::providers::resolve(&st.core.cfg, &model_id)?;
    let names = b.mcp_servers.unwrap_or_else(|| ws.mcp_servers.clone());
    if !names.is_empty() && !model.is_cli() {
        return Err(ApiError::bad_request(
            "MCP servers require a CLI provider (claude-code or codex)",
        ));
    }
    let mcp = crate::mcp::select(&st.core.cfg, &st.core.db, &names)
        .await
        .map_err(|e| ApiError::bad_request(format!("{e:#}")))?;
    let run = st
        .runs
        .start(
            &ws,
            StartRun {
                model,
                prompt: Zeroizing::new(b.prompt),
                mcp,
            },
        )
        .await?;
    Ok((StatusCode::ACCEPTED, Json(run)))
}

pub async fn list(State(st): State<AppState>, Path(id): Path<String>) -> ApiResult<Json<Value>> {
    let ws = load_workspace(&st, &id).await?;
    let runs = st.core.db.call(move |c| db::list_runs(c, &ws.id)).await?;
    Ok(data(runs))
}

pub async fn get(State(st): State<AppState>, Path(rid): Path<String>) -> ApiResult<Json<Value>> {
    let run = load_run(&st, &rid).await?;
    let events = st
        .runs
        .read_events(&run.id)
        .await
        .map_err(|e| ApiError::internal(e.context("run log integrity check failed")))?;
    let mut v = serde_json::to_value(&run).map_err(|e| ApiError::internal(e.into()))?;
    v["output"] = Value::String(crate::runs::output_text(&events));
    v["event_count"] = Value::from(events.len());
    Ok(Json(v))
}

pub async fn cancel(
    State(st): State<AppState>,
    Path(rid): Path<String>,
) -> ApiResult<impl IntoResponse> {
    let run = load_run(&st, &rid).await?;
    if !st.runs.cancel(&run.id) {
        return Err(ApiError::conflict("run is not active"));
    }
    Ok((
        StatusCode::ACCEPTED,
        Json(serde_json::json!({"id": run.id, "cancelling": true})),
    ))
}

fn sse_event(ev: &RunEvent) -> Event {
    Event::default()
        .id(ev.seq.to_string())
        .event(ev.kind.clone())
        .json_data(ev)
        .unwrap_or_else(|_| Event::default().comment("encode error"))
}

/// Replays the encrypted log after `Last-Event-ID`, then follows live
/// events until the run's `done` event.
pub async fn events(
    State(st): State<AppState>,
    Path(rid): Path<String>,
    headers: HeaderMap,
) -> ApiResult<Sse<impl Stream<Item = Result<Event, Infallible>>>> {
    let run = load_run(&st, &rid).await?;
    let mut last: u64 = headers
        .get("last-event-id")
        .and_then(|v| v.to_str().ok())
        .and_then(|v| v.parse().ok())
        .unwrap_or(0);
    // Subscribe before reading the log so nothing falls between the two.
    let mut live = st.runs.subscribe(&run.id);
    let past = st
        .runs
        .read_events(&run.id)
        .await
        .map_err(|e| ApiError::internal(e.context("run log integrity check failed")))?;
    let runs = st.runs.clone();
    let shutdown = st.core.shutdown.clone();
    let run_id = run.id.clone();
    let stream = async_stream::stream! {
        for ev in &past {
            if ev.seq <= last { continue; }
            last = ev.seq;
            yield Ok(sse_event(ev));
            if ev.is_done() { return; }
        }
        let Some(rx) = live.as_mut() else { return };
        loop {
            let item = tokio::select! {
                _ = shutdown.cancelled() => return,
                r = rx.recv() => r,
            };
            match item {
                Ok(ev) => {
                    if ev.seq <= last { continue; }
                    last = ev.seq;
                    let done = ev.is_done();
                    yield Ok(sse_event(&ev));
                    if done { return; }
                }
                Err(RecvError::Lagged(_)) | Err(RecvError::Closed) => {
                    // Catch up from disk.
                    let Ok(evs) = runs.read_events(&run_id).await else { return };
                    for ev in &evs {
                        if ev.seq <= last { continue; }
                        last = ev.seq;
                        yield Ok(sse_event(ev));
                        if ev.is_done() { return; }
                    }
                    if !runs.is_active(&run_id) { return; }
                }
            }
        }
    };
    Ok(Sse::new(stream).keep_alive(KeepAlive::new().interval(Duration::from_secs(15))))
}
