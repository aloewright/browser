// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

//! HTTP handlers for the ko-agentd v1 API.

pub mod artifacts;
pub mod chat;
pub mod mcp;
pub mod runs;
pub mod workspaces;

use axum::Json;
use axum::extract::rejection::{JsonRejection, QueryRejection};
use axum::extract::{FromRequest, FromRequestParts, Request, State};
use axum::http::StatusCode;
use axum::http::request::Parts;
use axum::response::IntoResponse;
use serde::Deserialize;
use serde::de::DeserializeOwned;
use serde_json::{Value, json};

use crate::db;
use crate::error::{ApiError, ApiResult};
use crate::server::AppState;

/// JSON body extractor with OpenAI-style error responses.
pub struct ApiJson<T>(pub T);

impl<T: DeserializeOwned, S: Send + Sync> FromRequest<S> for ApiJson<T> {
    type Rejection = ApiError;

    async fn from_request(req: Request, state: &S) -> Result<Self, Self::Rejection> {
        match Json::<T>::from_request(req, state).await {
            Ok(Json(v)) => Ok(Self(v)),
            Err(rej) => Err(json_rejection(rej)),
        }
    }
}

fn json_rejection(rej: JsonRejection) -> ApiError {
    if rej.status() == StatusCode::PAYLOAD_TOO_LARGE {
        return ApiError::too_large("request body too large");
    }
    let text = rej.body_text();
    ApiError::new(
        rej.status(),
        "invalid_request_error",
        crate::util::truncate(&text, 300).to_owned(),
    )
}

pub struct ApiQuery<T>(pub T);

impl<T: DeserializeOwned, S: Send + Sync> FromRequestParts<S> for ApiQuery<T> {
    type Rejection = ApiError;

    async fn from_request_parts(parts: &mut Parts, state: &S) -> Result<Self, Self::Rejection> {
        match axum::extract::Query::<T>::from_request_parts(parts, state).await {
            Ok(axum::extract::Query(v)) => Ok(Self(v)),
            Err(rej) => Err(query_rejection(rej)),
        }
    }
}

fn query_rejection(rej: QueryRejection) -> ApiError {
    ApiError::bad_request(crate::util::truncate(&rej.body_text(), 300).to_owned())
}

/// Validates a UUID path id (404 otherwise).
pub fn id_or_404(raw: &str, what: &str) -> ApiResult<String> {
    crate::util::parse_id(raw).ok_or_else(|| ApiError::not_found(what))
}

pub async fn load_workspace(st: &AppState, id: &str) -> ApiResult<db::Workspace> {
    let id = id_or_404(id, "workspace")?;
    st.core
        .db
        .call(move |c| db::get_workspace(c, &id))
        .await?
        .ok_or_else(|| ApiError::not_found("workspace"))
}

/// Free text (names, tags): bounded, no control characters.
pub fn check_text(field: &str, s: &str, max: usize) -> ApiResult<()> {
    if s.trim().is_empty() || s.len() > max || s.chars().any(char::is_control) {
        return Err(ApiError::bad_request(format!(
            "{field} must be 1..={max} bytes without control characters"
        )));
    }
    Ok(())
}

pub fn check_tags(tags: &[String]) -> ApiResult<()> {
    if tags.len() > 32 {
        return Err(ApiError::bad_request("at most 32 tags"));
    }
    for t in tags {
        check_text("tag", t, 64)?;
    }
    Ok(())
}

pub fn data<T: serde::Serialize>(items: T) -> Json<Value> {
    Json(json!({ "data": items }))
}

pub async fn health() -> Json<Value> {
    Json(json!({ "status": "ok", "version": crate::VERSION }))
}

pub async fn models(State(st): State<AppState>) -> Json<Value> {
    Json(json!({ "object": "list", "data": crate::providers::list_models(&st.core.cfg) }))
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct DeployBody {
    workspace_id: String,
}

pub async fn cloudflare_deploy(
    State(st): State<AppState>,
    ApiJson(body): ApiJson<DeployBody>,
) -> ApiResult<impl IntoResponse> {
    let ws = load_workspace(&st, &body.workspace_id).await?;
    let res = crate::cloudflare::deploy(&st.core, &ws).await;
    st.core
        .telemetry
        .capture("cloudflare_deploy", json!({ "ok": res.is_ok() }));
    let info = res?;
    let (id, i) = (ws.id.clone(), info.clone());
    st.core
        .db
        .call(move |c| db::set_cloudflare_agent(c, &id, &i))
        .await?;
    tracing::info!(script = %info.script_name, "deployed cloudflare agent");
    Ok((
        StatusCode::CREATED,
        Json(json!({
            "workspace_id": ws.id,
            "script_name": info.script_name,
            "url": info.url,
            "deployed_at": crate::util::rfc3339(info.deployed_at),
        })),
    ))
}
