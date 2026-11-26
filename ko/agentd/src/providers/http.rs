// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

//! OpenAI-compatible HTTP upstreams: Cloudflare AI Gateway (compat endpoint)
//! and NanoGPT. Requests and responses pass through (tools/tool_calls
//! included); only `model` is rewritten.

use std::time::Duration;

use axum::http::StatusCode;
use futures::{Stream, StreamExt};
use reqwest::Url;
use reqwest::header::{HeaderName, HeaderValue};
use serde_json::{Value, json};
use tokio::sync::mpsc;
use tokio_util::sync::CancellationToken;
use zeroize::Zeroizing;

use super::{GenEvent, ModelRef};
use crate::config::{Config, validate_upstream_url};
use crate::error::ApiError;
use crate::secrets::SecretStore;

const MAX_RESPONSE: usize = 16 * 1024 * 1024;
const MAX_SSE_LINE: usize = 4 * 1024 * 1024;

pub struct Upstream {
    pub provider: &'static str,
    pub url: Url,
    pub headers: Vec<(HeaderName, Zeroizing<String>)>,
    /// Model id sent upstream.
    pub model: String,
    pub timeout: Duration,
}

fn unavailable(e: anyhow::Error) -> ApiError {
    ApiError::unavailable(crate::telemetry::scrub(&format!("{e:#}")))
}

/// Builds the upstream for an HTTP model. Secrets come from the Keychain.
pub async fn upstream(
    cfg: &Config,
    secrets: &SecretStore,
    model: &ModelRef,
) -> Result<Upstream, ApiError> {
    let net = &cfg.network;
    match model {
        ModelRef::CfGateway { provider, model } => {
            let c = cfg
                .providers
                .cf_gateway
                .as_ref()
                .ok_or_else(|| ApiError::unavailable("cf-gateway is not configured"))?;
            let raw = format!(
                "{}/{}/{}/compat/chat/completions",
                c.base_url.trim_end_matches('/'),
                c.account_id,
                c.gateway
            );
            let url = validate_upstream_url(&raw, net).map_err(unavailable)?;
            let mut headers = Vec::new();
            if let Some(name) = &c.aig_token_secret
                && let Some(tok) = secrets.get_async(name).await.map_err(unavailable)?
            {
                headers.push((
                    HeaderName::from_static("cf-aig-authorization"),
                    Zeroizing::new(format!("Bearer {}", tok.as_str())),
                ));
            }
            let key_name = format!("{}{}", c.provider_key_secret_prefix, provider);
            if crate::util::valid_secret_name(&key_name)
                && let Some(key) = secrets.get_async(&key_name).await.map_err(unavailable)?
            {
                headers.push((
                    reqwest::header::AUTHORIZATION,
                    Zeroizing::new(format!("Bearer {}", key.as_str())),
                ));
            }
            if headers.is_empty() {
                return Err(ApiError::unavailable(format!(
                    "cf-gateway has no credentials: set secret {key_name:?} or the AI Gateway token"
                )));
            }
            Ok(Upstream {
                provider: "cf-gateway",
                url,
                headers,
                model: format!("{provider}/{model}"),
                timeout: Duration::from_secs(c.timeout_secs),
            })
        }
        ModelRef::NanoGpt { model } => {
            let c = cfg
                .providers
                .nanogpt
                .as_ref()
                .ok_or_else(|| ApiError::unavailable("nanogpt is not configured"))?;
            let raw = format!("{}/chat/completions", c.base_url.trim_end_matches('/'));
            let url = validate_upstream_url(&raw, net).map_err(unavailable)?;
            let key = secrets
                .require_async(&c.api_key_secret)
                .await
                .map_err(unavailable)?;
            Ok(Upstream {
                provider: "nanogpt",
                url,
                headers: vec![(
                    reqwest::header::AUTHORIZATION,
                    Zeroizing::new(format!("Bearer {}", key.as_str())),
                )],
                model: model.clone(),
                timeout: Duration::from_secs(c.timeout_secs),
            })
        }
        _ => Err(ApiError::bad_request("not an HTTP provider")),
    }
}

fn request(
    client: &reqwest::Client,
    up: &Upstream,
    body: &Value,
) -> Result<reqwest::RequestBuilder, ApiError> {
    let mut rb = client.post(up.url.clone()).json(body);
    for (k, v) in &up.headers {
        let mut hv = HeaderValue::from_str(v)
            .map_err(|_| ApiError::unavailable("invalid credential value"))?;
        hv.set_sensitive(true);
        rb = rb.header(k, hv);
    }
    Ok(rb)
}

fn transport_error(up: &Upstream, e: reqwest::Error) -> ApiError {
    let status = if e.is_timeout() {
        StatusCode::GATEWAY_TIMEOUT
    } else {
        StatusCode::BAD_GATEWAY
    };
    let msg = crate::telemetry::scrub(&e.without_url().to_string());
    ApiError::new(status, "provider_error", format!("{}: {msg}", up.provider))
}

async fn read_capped(resp: reqwest::Response, max: usize) -> Result<Vec<u8>, String> {
    let mut out = Vec::new();
    let mut s = resp.bytes_stream();
    while let Some(chunk) = s.next().await {
        let chunk = chunk.map_err(|e| e.without_url().to_string())?;
        if out.len() + chunk.len() > max {
            return Err("response too large".into());
        }
        out.extend_from_slice(&chunk);
    }
    Ok(out)
}

async fn status_error(up: &Upstream, resp: reqwest::Response) -> ApiError {
    let status = resp.status();
    let body = read_capped(resp, 64 * 1024).await.unwrap_or_default();
    let detail = serde_json::from_slice::<Value>(&body)
        .ok()
        .and_then(|v| {
            v["error"]["message"]
                .as_str()
                .or_else(|| v["error"].as_str())
                .or_else(|| v["message"].as_str())
                .map(str::to_owned)
        })
        .unwrap_or_default();
    let detail = crate::telemetry::scrub(crate::util::truncate(&detail, 500));
    let mapped = match status.as_u16() {
        400 | 413 | 422 => StatusCode::BAD_REQUEST,
        429 => StatusCode::TOO_MANY_REQUESTS,
        _ => StatusCode::BAD_GATEWAY,
    };
    ApiError::new(
        mapped,
        "provider_error",
        format!("{} returned {}: {detail}", up.provider, status.as_u16()),
    )
}

/// Client body with `model` swapped for the upstream id.
pub fn upstream_body(mut body: Value, up: &Upstream, stream: bool) -> Value {
    if let Some(o) = body.as_object_mut() {
        o.insert("model".into(), json!(up.model));
        o.insert("stream".into(), json!(stream));
    }
    body
}

pub async fn post_json(
    client: &reqwest::Client,
    up: &Upstream,
    body: &Value,
    our_model: &str,
) -> Result<Value, ApiError> {
    let resp = request(client, up, body)?
        .timeout(up.timeout)
        .send()
        .await
        .map_err(|e| transport_error(up, e))?;
    if !resp.status().is_success() {
        return Err(status_error(up, resp).await);
    }
    let bytes = read_capped(resp, MAX_RESPONSE).await.map_err(|e| {
        ApiError::provider(format!("{}: {}", up.provider, crate::telemetry::scrub(&e)))
    })?;
    let mut v: Value = serde_json::from_slice(&bytes)
        .map_err(|_| ApiError::provider(format!("{} returned invalid JSON", up.provider)))?;
    if let Some(o) = v.as_object_mut() {
        o.insert("model".into(), json!(our_model));
    }
    Ok(v)
}

pub enum SseItem {
    Data(Value),
    Done,
}

/// Parses an upstream SSE body into JSON data payloads.
pub fn sse_items<S>(bytes: S) -> impl Stream<Item = Result<SseItem, String>>
where
    S: Stream<Item = Result<bytes::Bytes, reqwest::Error>> + Send + 'static,
{
    async_stream::stream! {
        let mut buf: Vec<u8> = Vec::new();
        futures::pin_mut!(bytes);
        while let Some(chunk) = bytes.next().await {
            let chunk = match chunk {
                Ok(c) => c,
                Err(e) => { yield Err(e.without_url().to_string()); return; }
            };
            buf.extend_from_slice(&chunk);
            while let Some(pos) = buf.iter().position(|&b| b == b'\n') {
                let line: Vec<u8> = buf.drain(..=pos).collect();
                let line = String::from_utf8_lossy(&line);
                let line = line.trim_end_matches(['\r', '\n']);
                let Some(data) = line.strip_prefix("data:") else { continue };
                let data = data.trim_start();
                if data == "[DONE]" {
                    yield Ok(SseItem::Done);
                    return;
                }
                match serde_json::from_str::<Value>(data) {
                    Ok(v) => yield Ok(SseItem::Data(v)),
                    Err(_) => { yield Err("invalid JSON in upstream stream".into()); return; }
                }
            }
            if buf.len() > MAX_SSE_LINE {
                yield Err("upstream stream line too long".into());
                return;
            }
        }
    }
}

pub async fn post_stream(
    client: &reqwest::Client,
    up: &Upstream,
    body: &Value,
) -> Result<impl Stream<Item = Result<SseItem, String>> + use<>, ApiError> {
    let resp = request(client, up, body)?
        .send()
        .await
        .map_err(|e| transport_error(up, e))?;
    if !resp.status().is_success() {
        return Err(status_error(up, resp).await);
    }
    Ok(sse_items(resp.bytes_stream()))
}

/// Runs a single-prompt completion and maps it to provider events (agent
/// runs on HTTP providers).
pub async fn generate(
    client: reqwest::Client,
    up: Upstream,
    prompt: &str,
    cancel: CancellationToken,
) -> Result<mpsc::Receiver<GenEvent>, ApiError> {
    let body = json!({
        "model": up.model,
        "messages": [{"role": "user", "content": prompt}],
        "stream": true,
        "stream_options": {"include_usage": true},
    });
    let stream = post_stream(&client, &up, &body).await?;
    let (tx, rx) = mpsc::channel(64);
    let provider = up.provider;
    tokio::spawn(async move {
        futures::pin_mut!(stream);
        let mut finish = None;
        loop {
            let item = tokio::select! {
                _ = cancel.cancelled() => {
                    let _ = tx.send(GenEvent::Error { message: "cancelled".into() }).await;
                    return;
                }
                i = stream.next() => i,
            };
            let ev = match item {
                None | Some(Ok(SseItem::Done)) => {
                    let _ = tx
                        .send(GenEvent::Done {
                            finish_reason: finish.unwrap_or_else(|| "stop".into()),
                        })
                        .await;
                    return;
                }
                Some(Err(e)) => {
                    let _ = tx
                        .send(GenEvent::Error {
                            message: format!("{provider}: {}", crate::telemetry::scrub(&e)),
                        })
                        .await;
                    return;
                }
                Some(Ok(SseItem::Data(v))) => v,
            };
            if let Some(msg) = ev["error"]["message"].as_str() {
                let _ = tx
                    .send(GenEvent::Error {
                        message: format!("{provider}: {}", crate::telemetry::scrub(msg)),
                    })
                    .await;
                return;
            }
            if let Some(u) = ev.get("usage").filter(|u| u.is_object()) {
                let _ = tx
                    .send(GenEvent::Usage {
                        prompt_tokens: u["prompt_tokens"].as_u64().unwrap_or(0),
                        completion_tokens: u["completion_tokens"].as_u64().unwrap_or(0),
                    })
                    .await;
            }
            let choice = &ev["choices"][0];
            if let Some(t) = choice["delta"]["content"]
                .as_str()
                .filter(|t| !t.is_empty())
                && tx
                    .send(GenEvent::Text { text: t.to_owned() })
                    .await
                    .is_err()
            {
                return;
            }
            if let Some(f) = choice["finish_reason"].as_str() {
                finish = Some(f.to_owned());
            }
        }
    });
    Ok(rx)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn parses_sse() {
        let chunks: Vec<Result<bytes::Bytes, reqwest::Error>> = vec![
            Ok(bytes::Bytes::from_static(
                b": keepalive\n\ndata: {\"a\":1}\n\nda",
            )),
            Ok(bytes::Bytes::from_static(
                b"ta: {\"a\":2}\r\n\r\ndata: [DONE]\n\n",
            )),
        ];
        let items: Vec<_> = sse_items(futures::stream::iter(chunks)).collect().await;
        assert_eq!(items.len(), 3);
        assert!(matches!(&items[0], Ok(SseItem::Data(v)) if v["a"] == 1));
        assert!(matches!(&items[1], Ok(SseItem::Data(v)) if v["a"] == 2));
        assert!(matches!(&items[2], Ok(SseItem::Done)));
    }

    #[test]
    fn body_rewrite_keeps_tools() {
        let up = Upstream {
            provider: "nanogpt",
            url: Url::parse("https://nano-gpt.com/api/v1/chat/completions").unwrap(),
            headers: vec![],
            model: "gpt-4o".into(),
            timeout: Duration::from_secs(1),
        };
        let b = upstream_body(
            json!({"model": "nanogpt/gpt-4o", "messages": [], "tools": [{"type": "function"}]}),
            &up,
            true,
        );
        assert_eq!(b["model"], "gpt-4o");
        assert_eq!(b["stream"], true);
        assert_eq!(b["tools"][0]["type"], "function");
    }
}
