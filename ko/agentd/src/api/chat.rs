// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

//! POST /v1/chat/completions (OpenAI-compatible, SSE when stream=true).

use std::convert::Infallible;
use std::time::Duration;

use axum::Json;
use axum::extract::State;
use axum::response::sse::{Event, KeepAlive, Sse};
use axum::response::{IntoResponse, Response};
use futures::StreamExt;
use serde_json::{Value, json};
use zeroize::Zeroizing;

use super::ApiJson;
use crate::error::{ApiError, ApiResult};
use crate::providers::http::{self, SseItem};
use crate::providers::openai::{ChunkBuilder, aggregate, error_chunk};
use crate::providers::{GenEvent, GenParams, ModelRef};
use crate::server::AppState;

fn data_event(v: &Value) -> Event {
    Event::default().data(v.to_string())
}

fn done_event() -> Event {
    Event::default().data("[DONE]")
}

fn sse<S>(s: S) -> Response
where
    S: futures::Stream<Item = Result<Event, Infallible>> + Send + 'static,
{
    Sse::new(s)
        .keep_alive(KeepAlive::new().interval(Duration::from_secs(15)))
        .into_response()
}

pub async fn completions(
    State(st): State<AppState>,
    ApiJson(body): ApiJson<Value>,
) -> ApiResult<Response> {
    if !body.is_object() {
        return Err(ApiError::bad_request("body must be a JSON object"));
    }
    let model_id = body["model"]
        .as_str()
        .ok_or_else(|| ApiError::bad_request("model is required"))?
        .to_owned();
    let stream = body["stream"].as_bool().unwrap_or(false);
    let model = crate::providers::resolve(&st.core.cfg, &model_id)?;
    st.core.telemetry.capture(
        "chat_completion",
        json!({"provider": model.provider(), "stream": stream}),
    );
    let shutdown = st.core.shutdown.clone();
    if model.is_cli() {
        cli_completion(st, model, body, stream).await
    } else {
        let up = http::upstream(&st.core.cfg, &st.core.secrets, &model).await?;
        let upstream_body = http::upstream_body(body, &up, stream);
        if !stream {
            let v = http::post_json(&st.core.http, &up, &upstream_body, &model_id).await?;
            return Ok(Json(v).into_response());
        }
        let items = http::post_stream(&st.core.http, &up, &upstream_body, up.timeout).await?;
        let s = async_stream::stream! {
            futures::pin_mut!(items);
            let mut finished = false;
            loop {
                let item = tokio::select! {
                    _ = shutdown.cancelled() => None,
                    i = items.next() => i,
                };
                match item {
                    Some(Ok(SseItem::Done)) => break,
                    None => {
                        // Don't pass a truncated answer off as complete.
                        if !finished && !shutdown.is_cancelled() {
                            yield Ok(data_event(&error_chunk(http::EARLY_EOF)));
                        }
                        break;
                    }
                    Some(Ok(SseItem::Data(mut v))) => {
                        if v["choices"]
                            .as_array()
                            .is_some_and(|c| c.iter().any(|c| c["finish_reason"].is_string()))
                        {
                            finished = true;
                        }
                        if let Some(o) = v.as_object_mut()
                            && o.contains_key("model")
                        {
                            o.insert("model".into(), json!(model_id));
                        }
                        yield Ok(data_event(&v));
                    }
                    Some(Err(e)) => {
                        yield Ok(data_event(&error_chunk(&crate::telemetry::scrub(&e))));
                        break;
                    }
                }
            }
            yield Ok(done_event());
        };
        Ok(sse(s))
    }
}

async fn cli_completion(
    st: AppState,
    model: ModelRef,
    body: Value,
    stream: bool,
) -> ApiResult<Response> {
    let messages = body["messages"]
        .as_array()
        .ok_or_else(|| ApiError::bad_request("messages must be an array"))?;
    let prompt = Zeroizing::new(crate::providers::prompt::render(messages)?);
    let cancel = st.core.shutdown.child_token();
    let mut generation = crate::providers::generate(
        &st.core,
        GenParams {
            model: &model,
            prompt,
            mcp: &[],
            cwd: None,
            agent_run: false,
            timeout: None,
            cancel: cancel.clone(),
        },
    )
    .await?;
    let id = model.id();
    if !stream {
        let mut events = Vec::new();
        while let Some(ev) = generation.events.recv().await {
            let terminal = ev.is_terminal();
            events.push(ev);
            if terminal {
                break;
            }
        }
        return Ok(Json(aggregate(&id, &events)?).into_response());
    }
    let s = async_stream::stream! {
        // Dropping the stream (client gone) drops the generation and kills the CLI.
        let _guard = cancel.drop_guard();
        let mut b = ChunkBuilder::new(&id);
        while let Some(ev) = generation.events.recv().await {
            let terminal = ev.is_terminal();
            if let GenEvent::Error { message } = &ev {
                yield Ok(data_event(&error_chunk(&crate::telemetry::scrub(message))));
            } else if let Some(chunk) = b.event(&ev) {
                yield Ok(data_event(&chunk));
            }
            if terminal { break; }
        }
        yield Ok(done_event());
    };
    Ok(sse(s))
}
