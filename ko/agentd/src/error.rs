// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

use axum::Json;
use axum::http::StatusCode;
use axum::response::{IntoResponse, Response};
use serde_json::json;

/// Attached to 5xx responses so the telemetry layer can report them.
#[derive(Clone, Debug)]
pub struct ErrorReport(pub String);

/// OpenAI-style error: {"error": {"message", "type"}}.
#[derive(Debug)]
pub struct ApiError {
    pub status: StatusCode,
    pub kind: &'static str,
    pub message: String,
}

pub type ApiResult<T> = Result<T, ApiError>;

impl ApiError {
    pub fn new(status: StatusCode, kind: &'static str, message: impl Into<String>) -> Self {
        Self {
            status,
            kind,
            message: message.into(),
        }
    }
    pub fn bad_request(m: impl Into<String>) -> Self {
        Self::new(StatusCode::BAD_REQUEST, "invalid_request_error", m)
    }
    pub fn not_found(what: &str) -> Self {
        Self::new(
            StatusCode::NOT_FOUND,
            "not_found_error",
            format!("{what} not found"),
        )
    }
    pub fn unauthorized() -> Self {
        Self::new(
            StatusCode::UNAUTHORIZED,
            "authentication_error",
            "missing or invalid bearer token",
        )
    }
    pub fn forbidden(m: impl Into<String>) -> Self {
        Self::new(StatusCode::FORBIDDEN, "permission_error", m)
    }
    pub fn conflict(m: impl Into<String>) -> Self {
        Self::new(StatusCode::CONFLICT, "conflict_error", m)
    }
    pub fn unavailable(m: impl Into<String>) -> Self {
        Self::new(StatusCode::SERVICE_UNAVAILABLE, "unavailable_error", m)
    }
    pub fn provider(m: impl Into<String>) -> Self {
        Self::new(StatusCode::BAD_GATEWAY, "provider_error", m)
    }
    pub fn too_large(m: impl Into<String>) -> Self {
        Self::new(StatusCode::PAYLOAD_TOO_LARGE, "invalid_request_error", m)
    }
    pub fn internal(err: anyhow::Error) -> Self {
        let msg = crate::telemetry::scrub(&format!("{err:#}"));
        tracing::error!(error = %msg, "internal error");
        Self::new(
            StatusCode::INTERNAL_SERVER_ERROR,
            "internal_error",
            format!("internal error: {msg}"),
        )
    }
}

impl From<anyhow::Error> for ApiError {
    fn from(e: anyhow::Error) -> Self {
        match e.downcast::<ApiError>() {
            Ok(api) => api,
            Err(e) => Self::internal(e),
        }
    }
}

impl std::fmt::Display for ApiError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}: {}", self.kind, self.message)
    }
}

impl std::error::Error for ApiError {}

pub fn error_body(kind: &str, message: &str) -> serde_json::Value {
    json!({ "error": { "message": message, "type": kind } })
}

impl IntoResponse for ApiError {
    fn into_response(self) -> Response {
        let mut resp = (self.status, Json(error_body(self.kind, &self.message))).into_response();
        if self.status.is_server_error() {
            resp.extensions_mut()
                .insert(ErrorReport(format!("{}: {}", self.kind, self.message)));
        }
        resp
    }
}
