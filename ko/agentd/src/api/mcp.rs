// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

use axum::Json;
use axum::extract::{Path, State};
use axum::http::StatusCode;
use axum::response::IntoResponse;
use serde_json::Value;

use super::{ApiJson, data};
use crate::db;
use crate::error::{ApiError, ApiResult};
use crate::server::AppState;

pub async fn list(State(st): State<AppState>) -> ApiResult<Json<Value>> {
    let all = crate::mcp::list(&st.core.cfg, &st.core.db).await?;
    let secrets = st.core.secrets.clone();
    let views = tokio::task::spawn_blocking(move || {
        all.iter()
            .map(|(n, d, s)| crate::mcp::view(n, d, s, &secrets))
            .collect::<Vec<_>>()
    })
    .await
    .map_err(|e| ApiError::internal(e.into()))?;
    Ok(data(views))
}

/// Body: {"name": "...", "type": "http"|"stdio", ...}. Secret values are
/// referenced by Keychain name, never sent inline.
pub async fn create(
    State(st): State<AppState>,
    ApiJson(mut body): ApiJson<Value>,
) -> ApiResult<impl IntoResponse> {
    let name = body
        .as_object_mut()
        .and_then(|o| o.remove("name"))
        .and_then(|v| v.as_str().map(str::to_owned))
        .ok_or_else(|| ApiError::bad_request("name is required"))?;
    let def = crate::mcp::parse_api_definition(&st.core.cfg, &name, body)
        .map_err(|e| ApiError::bad_request(format!("{e:#}")))?;
    let existing = crate::mcp::list(&st.core.cfg, &st.core.db).await?;
    if existing.iter().any(|(n, _, _)| *n == name) {
        return Err(ApiError::conflict(format!(
            "MCP server {name:?} already exists"
        )));
    }
    let json = serde_json::to_string(&def).map_err(|e| ApiError::internal(e.into()))?;
    let n2 = name.clone();
    st.core
        .db
        .call(move |c| db::insert_mcp_server(c, &n2, &json, crate::util::now_ms()))
        .await?;
    let secrets = st.core.secrets.clone();
    let view = tokio::task::spawn_blocking(move || crate::mcp::view(&name, &def, "api", &secrets))
        .await
        .map_err(|e| ApiError::internal(e.into()))?;
    Ok((StatusCode::CREATED, Json(view)))
}

pub async fn remove(State(st): State<AppState>, Path(name): Path<String>) -> ApiResult<StatusCode> {
    if st.core.cfg.mcp.servers.contains_key(&name) {
        return Err(ApiError::conflict("server is defined in agentd.toml"));
    }
    if !crate::util::valid_ident(&name, 64) {
        return Err(ApiError::not_found("MCP server"));
    }
    if !st
        .core
        .db
        .call(move |c| db::delete_mcp_server(c, &name))
        .await?
    {
        return Err(ApiError::not_found("MCP server"));
    }
    Ok(StatusCode::NO_CONTENT)
}
