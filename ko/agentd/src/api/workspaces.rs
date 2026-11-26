// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

use axum::Json;
use axum::extract::{Path, State};
use axum::http::StatusCode;
use axum::response::IntoResponse;
use serde::{Deserialize, Deserializer};
use serde_json::{Value, json};

use super::{ApiJson, ApiQuery, check_tags, check_text, data, id_or_404, load_workspace};
use crate::db::{self, Workspace};
use crate::error::{ApiError, ApiResult};
use crate::server::AppState;

fn double_option<'de, D, T>(d: D) -> Result<Option<Option<T>>, D::Error>
where
    D: Deserializer<'de>,
    T: Deserialize<'de>,
{
    Ok(Some(Option::deserialize(d)?))
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct CreateBody {
    name: String,
    container_id: Option<String>,
    #[serde(default)]
    tags: Vec<String>,
    default_model: Option<String>,
    #[serde(default)]
    mcp_servers: Vec<String>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct PatchBody {
    name: Option<String>,
    #[serde(default, deserialize_with = "double_option")]
    container_id: Option<Option<String>>,
    tags: Option<Vec<String>>,
    #[serde(default, deserialize_with = "double_option")]
    default_model: Option<Option<String>>,
    mcp_servers: Option<Vec<String>>,
}

#[derive(Deserialize)]
pub struct ListQuery {
    q: Option<String>,
    tag: Option<String>,
}

/// Also stores `default_model` as the resolved id, so an alias such as
/// "claude-code/default" reads back as a listed model and `agents` can flag it.
async fn validate(st: &AppState, w: &mut Workspace) -> ApiResult<()> {
    check_text("name", &w.name, 200)?;
    if let Some(c) = &w.container_id {
        check_text("container_id", c, 200)?;
    }
    check_tags(&w.tags)?;
    if let Some(m) = &mut w.default_model {
        *m = crate::providers::resolve(&st.core.cfg, m)?.id();
    }
    if w.mcp_servers.len() > 32 {
        return Err(ApiError::bad_request("at most 32 MCP servers"));
    }
    crate::mcp::select(&st.core.cfg, &st.core.db, &w.mcp_servers)
        .await
        .map_err(|e| ApiError::bad_request(format!("{e:#}")))?;
    Ok(())
}

pub async fn list(
    State(st): State<AppState>,
    ApiQuery(q): ApiQuery<ListQuery>,
) -> ApiResult<Json<Value>> {
    let items = st
        .core
        .db
        .call(move |c| db::list_workspaces(c, q.q.as_deref(), q.tag.as_deref()))
        .await?;
    Ok(data(items))
}

pub async fn create(
    State(st): State<AppState>,
    ApiJson(b): ApiJson<CreateBody>,
) -> ApiResult<impl IntoResponse> {
    let now = crate::util::now_ms();
    let mut w = Workspace {
        id: crate::util::new_id(),
        name: b.name.trim().to_owned(),
        container_id: b.container_id,
        tags: b.tags,
        default_model: b.default_model,
        mcp_servers: b.mcp_servers,
        cloudflare_agent: None,
        created_at: now,
        updated_at: now,
    };
    validate(&st, &mut w).await?;
    let w2 = w.clone();
    st.core
        .db
        .call(move |c| db::insert_workspace(c, &w2))
        .await?;
    Ok((StatusCode::CREATED, Json(w)))
}

pub async fn get(State(st): State<AppState>, Path(id): Path<String>) -> ApiResult<Json<Workspace>> {
    Ok(Json(load_workspace(&st, &id).await?))
}

pub async fn patch(
    State(st): State<AppState>,
    Path(id): Path<String>,
    ApiJson(b): ApiJson<PatchBody>,
) -> ApiResult<Json<Workspace>> {
    let mut w = load_workspace(&st, &id).await?;
    if let Some(n) = b.name {
        w.name = n.trim().to_owned();
    }
    if let Some(c) = b.container_id {
        w.container_id = c;
    }
    if let Some(t) = b.tags {
        w.tags = t;
    }
    if let Some(m) = b.default_model {
        w.default_model = m;
    }
    if let Some(m) = b.mcp_servers {
        w.mcp_servers = m;
    }
    w.updated_at = crate::util::now_ms();
    validate(&st, &mut w).await?;
    let w2 = w.clone();
    st.core
        .db
        .call(move |c| db::update_workspace(c, &w2))
        .await?;
    Ok(Json(w))
}

/// Deletes the workspace, its artifacts, run logs and default work dir.
pub async fn remove(State(st): State<AppState>, Path(id): Path<String>) -> ApiResult<StatusCode> {
    let id = id_or_404(&id, "workspace")?;
    st.runs.cancel_workspace(&id);
    let id2 = id.clone();
    let (deleted, run_ids) = st
        .core
        .db
        .call(move |c| {
            let runs = db::run_ids_for_workspace(c, &id2)?;
            Ok((db::delete_workspace(c, &id2)?, runs))
        })
        .await?;
    if !deleted {
        return Err(ApiError::not_found("workspace"));
    }
    let core = st.core.clone();
    tokio::task::spawn_blocking(move || {
        let _ = std::fs::remove_dir_all(core.paths.artifacts_dir().join(&id));
        for r in run_ids {
            let _ = std::fs::remove_file(crate::runs::log_path(&core, &r));
        }
        if core.cfg.workspaces.workdir_root.is_none() {
            let _ = std::fs::remove_dir_all(core.paths.workspaces_dir().join(&id));
        }
    })
    .await
    .map_err(|e| ApiError::internal(e.into()))?;
    Ok(StatusCode::NO_CONTENT)
}

/// Agents available to this workspace: configured models plus its deployed
/// Cloudflare agent.
pub async fn agents(State(st): State<AppState>, Path(id): Path<String>) -> ApiResult<Json<Value>> {
    let w = load_workspace(&st, &id).await?;
    let mut items: Vec<Value> = crate::providers::list_models(&st.core.cfg)
        .into_iter()
        .map(|m| {
            let r = crate::providers::ModelRef::parse(&m.id);
            let cli = r.as_ref().is_some_and(|r| r.is_cli());
            json!({
                "id": m.id,
                "kind": if cli { "cli" } else { "http" },
                "provider": m.owned_by,
                "supports_mcp": cli,
                "default": w.default_model.as_deref() == Some(m.id.as_str()),
            })
        })
        .collect();
    if let Some(cf) = &w.cloudflare_agent {
        items.push(json!({
            "id": "cloudflare",
            "kind": "cloudflare",
            "provider": "cloudflare",
            "supports_mcp": false,
            "url": cf.url,
            "script_name": cf.script_name,
            "deployed_at": crate::util::rfc3339(cf.deployed_at),
        }));
    }
    Ok(data(items))
}
