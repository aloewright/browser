// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

//! Artifacts: metadata in SQLite, contents AES-256-GCM encrypted per file.

use axum::Json;
use axum::body::Body;
use axum::extract::{Path, State};
use axum::http::{HeaderValue, StatusCode, header};
use axum::response::{IntoResponse, Response};
use base64::Engine;
use base64::engine::general_purpose::STANDARD;
use serde::Deserialize;
use serde_json::{Value, json};
use zeroize::Zeroizing;

use super::{ApiJson, ApiQuery, check_tags, check_text, data, id_or_404, load_workspace};
use crate::crypto::artifact_aad;
use crate::db::{self, ArtifactMeta};
use crate::error::{ApiError, ApiResult};
use crate::server::AppState;

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct CreateBody {
    name: String,
    mime_type: Option<String>,
    #[serde(default)]
    tags: Vec<String>,
    content_base64: Option<String>,
    content_text: Option<String>,
}

#[derive(Deserialize)]
pub struct ListQuery {
    q: Option<String>,
    tag: Option<String>,
    mime_type: Option<String>,
}

#[derive(Deserialize)]
pub struct GetQuery {
    #[serde(default)]
    raw: bool,
}

fn valid_mime(m: &str) -> bool {
    let base = m.split(';').next().unwrap_or("").trim();
    let Some((t, s)) = base.split_once('/') else {
        return false;
    };
    let tok = |x: &str| {
        !x.is_empty()
            && x.bytes()
                .all(|b| b.is_ascii_alphanumeric() || b"!#$&^_.+-".contains(&b))
    };
    m.len() <= 127 && tok(t) && tok(s) && !m.chars().any(char::is_control)
}

fn blob_path(st: &AppState, ws: &str, aid: &str) -> std::path::PathBuf {
    st.core
        .paths
        .artifacts_dir()
        .join(ws)
        .join(format!("{aid}.bin"))
}

pub async fn list(
    State(st): State<AppState>,
    Path(id): Path<String>,
    ApiQuery(q): ApiQuery<ListQuery>,
) -> ApiResult<Json<Value>> {
    let ws = load_workspace(&st, &id).await?;
    let items = st
        .core
        .db
        .call(move |c| {
            db::list_artifacts(
                c,
                &ws.id,
                q.q.as_deref(),
                q.tag.as_deref(),
                q.mime_type.as_deref(),
            )
        })
        .await?;
    Ok(data(items))
}

pub async fn create(
    State(st): State<AppState>,
    Path(id): Path<String>,
    ApiJson(b): ApiJson<CreateBody>,
) -> ApiResult<impl IntoResponse> {
    let ws = load_workspace(&st, &id).await?;
    check_text("name", &b.name, 255)?;
    check_tags(&b.tags)?;
    let (content, default_mime) = match (b.content_base64, b.content_text) {
        (Some(b64), None) => (
            Zeroizing::new(
                STANDARD
                    .decode(b64.as_bytes())
                    .map_err(|_| ApiError::bad_request("content_base64 is not valid base64"))?,
            ),
            "application/octet-stream",
        ),
        (None, Some(t)) => (Zeroizing::new(t.into_bytes()), "text/plain; charset=utf-8"),
        _ => {
            return Err(ApiError::bad_request(
                "exactly one of content_base64 or content_text is required",
            ));
        }
    };
    if content.len() > st.core.cfg.server.max_artifact_bytes {
        return Err(ApiError::too_large("artifact too large"));
    }
    let mime = b.mime_type.unwrap_or_else(|| default_mime.to_owned());
    if !valid_mime(&mime) {
        return Err(ApiError::bad_request("invalid mime_type"));
    }
    let meta = ArtifactMeta {
        id: crate::util::new_id(),
        workspace_id: ws.id.clone(),
        name: b.name,
        mime_type: mime,
        size: content.len() as i64,
        tags: b.tags,
        created_at: crate::util::now_ms(),
    };
    let sealed = st
        .core
        .key
        .seal(&artifact_aad(&meta.workspace_id, &meta.id), &content)
        .map_err(ApiError::internal)?;
    let path = blob_path(&st, &meta.workspace_id, &meta.id);
    let dir = st.core.paths.artifacts_dir().join(&meta.workspace_id);
    let p2 = path.clone();
    tokio::task::spawn_blocking(move || -> anyhow::Result<()> {
        crate::paths::ensure_private_dir(&dir)?;
        crate::paths::write_private_file_atomic(&p2, &sealed)
    })
    .await
    .map_err(|e| ApiError::internal(e.into()))??;
    let m2 = meta.clone();
    if let Err(e) = st.core.db.call(move |c| db::insert_artifact(c, &m2)).await {
        let _ = std::fs::remove_file(&path);
        return Err(e.into());
    }
    Ok((StatusCode::CREATED, Json(meta)))
}

pub async fn get(
    State(st): State<AppState>,
    Path((id, aid)): Path<(String, String)>,
    ApiQuery(q): ApiQuery<GetQuery>,
) -> ApiResult<Response> {
    let ws = id_or_404(&id, "workspace")?;
    let aid = id_or_404(&aid, "artifact")?;
    let (w2, a2) = (ws.clone(), aid.clone());
    let meta = st
        .core
        .db
        .call(move |c| db::get_artifact(c, &w2, &a2))
        .await?
        .ok_or_else(|| ApiError::not_found("artifact"))?;
    let path = blob_path(&st, &ws, &aid);
    let sealed = tokio::fs::read(&path)
        .await
        .map_err(|_| ApiError::not_found("artifact content"))?;
    let content = st
        .core
        .key
        .open(&artifact_aad(&ws, &aid), &sealed)
        .map_err(|e| ApiError::internal(e.context("artifact integrity check failed")))?;
    if q.raw {
        let mut r = Response::new(Body::from(content.to_vec()));
        let h = r.headers_mut();
        h.insert(
            header::CONTENT_TYPE,
            HeaderValue::from_static("application/octet-stream"),
        );
        h.insert(
            header::CONTENT_DISPOSITION,
            HeaderValue::from_static("attachment"),
        );
        if let Ok(v) = HeaderValue::from_str(&meta.mime_type) {
            h.insert("x-ko-mime-type", v);
        }
        return Ok(r);
    }
    let mut v = serde_json::to_value(&meta).map_err(|e| ApiError::internal(e.into()))?;
    v["content_base64"] = json!(STANDARD.encode(&*content));
    Ok(Json(v).into_response())
}

pub async fn remove(
    State(st): State<AppState>,
    Path((id, aid)): Path<(String, String)>,
) -> ApiResult<StatusCode> {
    let ws = id_or_404(&id, "workspace")?;
    let aid = id_or_404(&aid, "artifact")?;
    let (w2, a2) = (ws.clone(), aid.clone());
    if !st
        .core
        .db
        .call(move |c| db::delete_artifact(c, &w2, &a2))
        .await?
    {
        return Err(ApiError::not_found("artifact"));
    }
    let _ = tokio::fs::remove_file(blob_path(&st, &ws, &aid)).await;
    Ok(StatusCode::NO_CONTENT)
}

#[cfg(test)]
mod tests {
    #[test]
    fn mime_validation() {
        assert!(super::valid_mime("text/plain; charset=utf-8"));
        assert!(super::valid_mime("application/vnd.example+json"));
        assert!(!super::valid_mime("text"));
        assert!(!super::valid_mime("text/plain\r\nx: y"));
    }
}
