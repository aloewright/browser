// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

//! Per-install bearer token, Host (DNS-rebinding) and Origin checks, CORS.

use std::sync::Arc;

use anyhow::{Context, Result, bail};
use axum::body::Body;
use axum::extract::{Request, State};
use axum::http::{HeaderMap, HeaderValue, Method, StatusCode, header};
use axum::middleware::Next;
use axum::response::{IntoResponse, Response};
use base64::Engine;
use base64::engine::general_purpose::URL_SAFE_NO_PAD;
use sha2::{Digest, Sha256};
use subtle::ConstantTimeEq;
use zeroize::Zeroizing;

use crate::error::ApiError;
use crate::paths::{
    Paths, create_private_file_exclusive, read_private_file, write_private_file_atomic,
};

/// The only origins allowed to call the daemon from a browser.
pub const ALLOWED_ORIGINS: [&str; 2] = ["chrome://ko-workspace", "chrome://ko-machines"];

const ALLOW_METHODS: &str = "GET, POST, PATCH, DELETE, OPTIONS";
const ALLOW_HEADERS: &str = "authorization, content-type, last-event-id";

pub struct Token(Zeroizing<String>);

impl Token {
    fn generate() -> Result<Zeroizing<String>> {
        let raw = Zeroizing::new(crate::util::random_bytes::<32>()?);
        Ok(Zeroizing::new(URL_SAFE_NO_PAD.encode(raw.as_slice())))
    }

    fn parse(bytes: &[u8]) -> Result<Self> {
        let s = std::str::from_utf8(bytes)
            .context("token file is not UTF-8")?
            .trim();
        if s.len() < 32
            || !s
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_')
        {
            bail!("token file is malformed; run `ko-agentd token rotate`");
        }
        crate::telemetry::register_secret(s);
        Ok(Self(Zeroizing::new(s.to_owned())))
    }

    /// Reads `<data>/agentd.token`, creating it (0600) on first run.
    pub fn load_or_create(paths: &Paths) -> Result<Self> {
        let p = paths.token();
        if !p.exists() {
            let t = Self::generate()?;
            let mut line = Zeroizing::new(t.as_bytes().to_vec());
            line.push(b'\n');
            create_private_file_exclusive(&p, &line)?;
        }
        Self::load(paths)
    }

    pub fn load(paths: &Paths) -> Result<Self> {
        let bytes = read_private_file(&paths.token()).context(
            "reading the agentd token (run `ko-agentd token rotate` if permissions were loose)",
        )?;
        Self::parse(&bytes)
    }

    pub fn rotate(paths: &Paths) -> Result<Self> {
        let t = Self::generate()?;
        let mut line = Zeroizing::new(t.as_bytes().to_vec());
        line.push(b'\n');
        write_private_file_atomic(&paths.token(), &line)?;
        Self::load(paths)
    }

    pub fn as_str(&self) -> &str {
        &self.0
    }

    /// Constant-time comparison over SHA-256 digests (hides length too).
    pub fn verify(&self, presented: &str) -> bool {
        let a = Sha256::digest(self.0.as_bytes());
        let b = Sha256::digest(presented.as_bytes());
        a.ct_eq(&b).into()
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Transport {
    Tcp { port: u16 },
    Unix,
}

#[derive(Clone)]
pub struct Guard {
    pub token: Arc<Token>,
    pub transport: Transport,
}

/// DNS-rebinding guard: TCP requests must name 127.0.0.1:<port> or
/// localhost:<port>. The Unix socket is not reachable from web content.
pub fn host_allowed(
    transport: Transport,
    headers: &HeaderMap,
    uri_authority: Option<&str>,
) -> bool {
    let Transport::Tcp { port } = transport else {
        return true;
    };
    let values: Vec<&HeaderValue> = headers.get_all(header::HOST).iter().collect();
    if values.len() != 1 {
        return false;
    }
    let Ok(host) = values[0].to_str() else {
        return false;
    };
    let ok = |h: &str| {
        let h = h.to_ascii_lowercase();
        h == format!("127.0.0.1:{port}") || h == format!("localhost:{port}")
    };
    ok(host) && uri_authority.is_none_or(ok)
}

pub fn origin_allowed(origin: &str) -> bool {
    ALLOWED_ORIGINS.contains(&origin)
}

fn bearer(headers: &HeaderMap) -> Option<&str> {
    let v = headers.get(header::AUTHORIZATION)?.to_str().ok()?;
    let (scheme, rest) = v.split_once(' ')?;
    scheme.eq_ignore_ascii_case("bearer").then(|| rest.trim())
}

fn apply_headers(resp: &mut Response, cors_origin: Option<&HeaderValue>) {
    let h = resp.headers_mut();
    h.insert(
        header::X_CONTENT_TYPE_OPTIONS,
        HeaderValue::from_static("nosniff"),
    );
    h.insert(header::CACHE_CONTROL, HeaderValue::from_static("no-store"));
    h.insert(
        header::REFERRER_POLICY,
        HeaderValue::from_static("no-referrer"),
    );
    h.insert(
        header::CONTENT_SECURITY_POLICY,
        HeaderValue::from_static("default-src 'none'; frame-ancestors 'none'; sandbox"),
    );
    h.insert(header::X_FRAME_OPTIONS, HeaderValue::from_static("DENY"));
    h.insert(header::VARY, HeaderValue::from_static("Origin"));
    if let Some(o) = cors_origin {
        h.insert(header::ACCESS_CONTROL_ALLOW_ORIGIN, o.clone());
    }
}

fn reject(err: ApiError, cors_origin: Option<&HeaderValue>) -> Response {
    let unauthorized = err.status == StatusCode::UNAUTHORIZED;
    let mut r = err.into_response();
    if unauthorized {
        r.headers_mut()
            .insert(header::WWW_AUTHENTICATE, HeaderValue::from_static("Bearer"));
    }
    apply_headers(&mut r, cors_origin);
    r
}

/// Order: Host, Origin/Sec-Fetch-Site, CORS preflight, bearer token.
pub async fn guard(State(g): State<Guard>, req: Request, next: Next) -> Response {
    let headers = req.headers();
    let authority = req.uri().authority().map(|a| a.as_str().to_owned());
    if !host_allowed(g.transport, headers, authority.as_deref()) {
        return reject(ApiError::forbidden("host not allowed"), None);
    }

    let cors_origin = match headers.get(header::ORIGIN) {
        None => None,
        Some(v) => match v.to_str() {
            Ok(o) if origin_allowed(o) => Some(v.clone()),
            _ => return reject(ApiError::forbidden("origin not allowed"), None),
        },
    };
    if cors_origin.is_none()
        && let Some(site) = headers.get("sec-fetch-site").and_then(|v| v.to_str().ok())
        && !matches!(site, "none" | "same-origin")
    {
        return reject(ApiError::forbidden("cross-site request not allowed"), None);
    }

    if req.method() == Method::OPTIONS {
        if cors_origin.is_some() && headers.contains_key(header::ACCESS_CONTROL_REQUEST_METHOD) {
            let mut r = Response::new(Body::empty());
            *r.status_mut() = StatusCode::NO_CONTENT;
            let h = r.headers_mut();
            h.insert(
                header::ACCESS_CONTROL_ALLOW_METHODS,
                HeaderValue::from_static(ALLOW_METHODS),
            );
            h.insert(
                header::ACCESS_CONTROL_ALLOW_HEADERS,
                HeaderValue::from_static(ALLOW_HEADERS),
            );
            h.insert(
                header::ACCESS_CONTROL_MAX_AGE,
                HeaderValue::from_static("600"),
            );
            // Chrome Private/Local Network Access preflight (allowed origins only).
            if headers
                .get("access-control-request-private-network")
                .is_some_and(|v| v == "true")
            {
                h.insert(
                    "access-control-allow-private-network",
                    HeaderValue::from_static("true"),
                );
            }
            apply_headers(&mut r, cors_origin.as_ref());
            return r;
        }
        return reject(ApiError::forbidden("preflight not allowed"), None);
    }

    if req.uri().path() != "/v1/health" {
        let ok = bearer(headers).is_some_and(|t| g.token.verify(t));
        if !ok {
            return reject(ApiError::unauthorized(), cors_origin.as_ref());
        }
    }

    let mut resp = next.run(req).await;
    apply_headers(&mut resp, cors_origin.as_ref());
    resp
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::PermissionsExt;

    fn hm(pairs: &[(&str, &str)]) -> HeaderMap {
        let mut h = HeaderMap::new();
        for (k, v) in pairs {
            h.append(
                header::HeaderName::from_bytes(k.as_bytes()).unwrap(),
                HeaderValue::from_str(v).unwrap(),
            );
        }
        h
    }

    #[test]
    fn token_lifecycle() {
        let t = tempfile::tempdir().unwrap();
        let paths = Paths::new(t.path());
        let tok = Token::load_or_create(&paths).unwrap();
        let mode = std::fs::metadata(paths.token())
            .unwrap()
            .permissions()
            .mode()
            & 0o777;
        assert_eq!(mode, 0o600);
        assert!(tok.verify(tok.as_str()));
        assert!(!tok.verify(""));
        assert!(!tok.verify(&tok.as_str()[1..]));
        assert!(!tok.verify(&format!("{}x", tok.as_str())));
        let again = Token::load_or_create(&paths).unwrap();
        assert_eq!(again.as_str(), tok.as_str());
        let rotated = Token::rotate(&paths).unwrap();
        assert_ne!(rotated.as_str(), tok.as_str());
        std::fs::set_permissions(paths.token(), std::fs::Permissions::from_mode(0o644)).unwrap();
        assert!(Token::load(&paths).is_err());
    }

    #[test]
    fn host_guard() {
        let tcp = Transport::Tcp { port: 47821 };
        assert!(host_allowed(tcp, &hm(&[("host", "127.0.0.1:47821")]), None));
        assert!(host_allowed(tcp, &hm(&[("host", "LOCALHOST:47821")]), None));
        assert!(!host_allowed(tcp, &hm(&[("host", "127.0.0.1")]), None));
        assert!(!host_allowed(tcp, &hm(&[("host", "127.0.0.1:1")]), None));
        assert!(!host_allowed(
            tcp,
            &hm(&[("host", "evil.example:47821")]),
            None
        ));
        assert!(!host_allowed(tcp, &hm(&[]), None));
        assert!(!host_allowed(
            tcp,
            &hm(&[("host", "127.0.0.1:47821"), ("host", "evil.example")]),
            None
        ));
        assert!(!host_allowed(
            tcp,
            &hm(&[("host", "127.0.0.1:47821")]),
            Some("evil.example:47821")
        ));
        assert!(host_allowed(Transport::Unix, &hm(&[]), None));
    }

    #[test]
    fn origins() {
        assert!(origin_allowed("chrome://ko-workspace"));
        assert!(origin_allowed("chrome://ko-machines"));
        assert!(!origin_allowed("chrome://settings"));
        assert!(!origin_allowed("chrome://ko-workspace.evil"));
        assert!(!origin_allowed("https://example.com"));
        assert!(!origin_allowed("null"));
    }

    #[test]
    fn bearer_parsing() {
        assert_eq!(bearer(&hm(&[("authorization", "Bearer abc")])), Some("abc"));
        assert_eq!(
            bearer(&hm(&[("authorization", "bearer  abc ")])),
            Some("abc")
        );
        assert_eq!(bearer(&hm(&[("authorization", "Basic abc")])), None);
        assert_eq!(bearer(&hm(&[])), None);
    }
}
