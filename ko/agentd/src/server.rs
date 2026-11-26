// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

//! Daemon wiring: listeners (127.0.0.1 TCP + 0600 Unix socket), router,
//! graceful shutdown.

use std::net::SocketAddr;
use std::os::unix::fs::{FileTypeExt, PermissionsExt};
use std::path::PathBuf;
use std::sync::Arc;
use std::time::Duration;

use anyhow::{Context, Result, bail};
use axum::Router;
use axum::extract::{DefaultBodyLimit, Request, State};
use axum::middleware::{Next, from_fn_with_state};
use axum::response::{IntoResponse, Response};
use axum::routing::{delete, get, post};
use hyper_util::rt::{TokioIo, TokioTimer};
use hyper_util::server::graceful::GracefulShutdown;
use hyper_util::service::TowerToHyperService;
use tokio::io::{AsyncRead, AsyncWrite};
use tokio::net::{TcpListener, UnixListener};
use tokio_util::sync::CancellationToken;

use crate::api;
use crate::auth::{Guard, Token, Transport};
use crate::config::Config;
use crate::crypto::{StorageKey, load_or_create_storage_key};
use crate::db::{self, Db};
use crate::error::{ApiError, ErrorReport};
use crate::paths::{Paths, write_private_file_atomic};
use crate::runs::RunManager;
use crate::secrets::SecretStore;
use crate::telemetry::Telemetry;

/// Shared daemon state.
pub struct Core {
    pub cfg: Arc<Config>,
    pub paths: Paths,
    pub db: Db,
    pub secrets: SecretStore,
    pub key: StorageKey,
    pub http: reqwest::Client,
    pub telemetry: Telemetry,
    pub shutdown: CancellationToken,
}

#[derive(Clone)]
pub struct AppState {
    pub core: Arc<Core>,
    pub runs: Arc<RunManager>,
}

pub fn build_http_client(cfg: &Config) -> Result<reqwest::Client> {
    let mut b = reqwest::Client::builder()
        .user_agent(concat!("ko-agentd/", env!("CARGO_PKG_VERSION")))
        .connect_timeout(Duration::from_secs(10))
        .read_timeout(Duration::from_secs(180))
        .redirect(reqwest::redirect::Policy::none())
        .no_proxy()
        .https_only(!cfg.network.allow_loopback_http);
    if let Some(p) = &cfg.network.proxy {
        b = b.proxy(reqwest::Proxy::all(p).context("network.proxy")?);
    }
    b.build().context("building HTTP client")
}

async fn timeout_layer(State(d): State<Duration>, req: Request, next: Next) -> Response {
    match tokio::time::timeout(d, next.run(req)).await {
        Ok(r) => r,
        Err(_) => ApiError::new(
            axum::http::StatusCode::GATEWAY_TIMEOUT,
            "timeout_error",
            "request timed out",
        )
        .into_response(),
    }
}

async fn report_layer(State(t): State<Telemetry>, req: Request, next: Next) -> Response {
    let resp = next.run(req).await;
    if let Some(ErrorReport(msg)) = resp.extensions().get::<ErrorReport>() {
        t.report_error("api", msg);
    }
    resp
}

async fn not_found() -> ApiError {
    ApiError::not_found("route")
}

pub fn build_router(state: AppState, guard: Guard) -> Router {
    let s = &state.core.cfg.server;
    let timeout = Duration::from_secs(s.request_timeout_secs);
    // base64 inflates artifact uploads by 4/3.
    let artifact_limit = s.max_artifact_bytes / 3 * 4 + 64 * 1024;
    let standard = Router::new()
        .route("/v1/health", get(api::health))
        .route("/v1/models", get(api::models))
        .route(
            "/v1/workspaces",
            get(api::workspaces::list).post(api::workspaces::create),
        )
        .route(
            "/v1/workspaces/{id}",
            get(api::workspaces::get)
                .patch(api::workspaces::patch)
                .delete(api::workspaces::remove),
        )
        .route(
            "/v1/workspaces/{id}/artifacts",
            get(api::artifacts::list)
                .post(api::artifacts::create)
                .layer(DefaultBodyLimit::max(artifact_limit)),
        )
        .route(
            "/v1/workspaces/{id}/artifacts/{aid}",
            get(api::artifacts::get).delete(api::artifacts::remove),
        )
        .route("/v1/workspaces/{id}/agents", get(api::workspaces::agents))
        .route(
            "/v1/workspaces/{id}/runs",
            get(api::runs::list).post(api::runs::start),
        )
        .route("/v1/runs/{rid}", get(api::runs::get))
        .route("/v1/runs/{rid}/cancel", post(api::runs::cancel))
        .route(
            "/v1/mcp/servers",
            get(api::mcp::list).post(api::mcp::create),
        )
        .route("/v1/mcp/servers/{name}", delete(api::mcp::remove))
        .route("/v1/cloudflare/agents", post(api::cloudflare_deploy))
        .layer(from_fn_with_state(timeout, timeout_layer));
    let streaming = Router::new()
        .route(
            "/v1/chat/completions",
            post(api::chat::completions).layer(DefaultBodyLimit::max(s.max_chat_body_bytes)),
        )
        .route("/v1/runs/{rid}/events", get(api::runs::events));
    let telemetry = state.core.telemetry.clone();
    Router::new()
        .merge(standard)
        .merge(streaming)
        .fallback(not_found)
        .layer(DefaultBodyLimit::max(s.max_body_bytes))
        .with_state(state)
        .layer(from_fn_with_state(telemetry, report_layer))
        .layer(from_fn_with_state(guard, crate::auth::guard))
}

trait Accept: Send + 'static {
    type Io: AsyncRead + AsyncWrite + Unpin + Send + 'static;
    fn accept_io(&self) -> impl std::future::Future<Output = std::io::Result<Self::Io>> + Send;
}

impl Accept for TcpListener {
    type Io = tokio::net::TcpStream;
    async fn accept_io(&self) -> std::io::Result<Self::Io> {
        let (s, _) = self.accept().await?;
        let _ = s.set_nodelay(true);
        Ok(s)
    }
}

impl Accept for UnixListener {
    type Io = tokio::net::UnixStream;
    async fn accept_io(&self) -> std::io::Result<Self::Io> {
        let (s, _) = self.accept().await?;
        Ok(s)
    }
}

async fn serve_listener<L: Accept>(
    listener: L,
    router: Router,
    shutdown: CancellationToken,
    header_timeout: Duration,
) {
    let graceful = GracefulShutdown::new();
    let mut builder = hyper::server::conn::http1::Builder::new();
    builder
        .timer(TokioTimer::new())
        .header_read_timeout(header_timeout)
        .max_buf_size(64 * 1024);
    loop {
        tokio::select! {
            r = listener.accept_io() => match r {
                Ok(io) => {
                    let svc = TowerToHyperService::new(router.clone());
                    let conn = builder.serve_connection(TokioIo::new(io), svc);
                    let fut = graceful.watch(conn);
                    tokio::spawn(async move {
                        if let Err(e) = fut.await {
                            tracing::debug!(error = %e, "connection error");
                        }
                    });
                }
                Err(e) => {
                    tracing::warn!(error = %e, "accept failed");
                    tokio::time::sleep(Duration::from_millis(100)).await;
                }
            },
            _ = shutdown.cancelled() => break,
        }
    }
    drop(listener);
    if tokio::time::timeout(Duration::from_secs(10), graceful.shutdown())
        .await
        .is_err()
    {
        tracing::warn!("connections still open after 10s; closing");
    }
}

#[derive(Debug, Clone, Default)]
pub struct ServeOptions {
    pub port: Option<u16>,
    pub unix_socket: Option<bool>,
}

pub struct RunningServer {
    pub tcp_addr: SocketAddr,
    pub socket_path: Option<PathBuf>,
    pub state: AppState,
    shutdown: CancellationToken,
    join: tokio::task::JoinHandle<()>,
}

impl RunningServer {
    pub fn shutdown(&self) {
        self.shutdown.cancel();
    }

    pub async fn wait(self) -> Result<()> {
        self.join.await.context("server task panicked")
    }
}

fn bind_unix(path: &PathBuf) -> Result<UnixListener> {
    if let Ok(meta) = std::fs::symlink_metadata(path) {
        if !meta.file_type().is_socket() {
            bail!("{} exists and is not a socket", path.display());
        }
        std::fs::remove_file(path)?;
    }
    let l = UnixListener::bind(path).with_context(|| format!("binding {}", path.display()))?;
    std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o600))?;
    Ok(l)
}

/// Opens state, binds listeners and starts serving in the background.
pub async fn start(paths: Paths, opts: ServeOptions) -> Result<RunningServer> {
    paths.ensure()?;
    paths.clean_tmp()?;
    let cfg = Arc::new(Config::load(&paths.config())?);
    let token = Arc::new(Token::load_or_create(&paths)?);
    let secrets = SecretStore::open(cfg.secret_backend()?, &paths)?;
    let db = Db::open(&paths.db())?;
    let now = crate::util::now_ms();
    let interrupted = db.call(move |c| db::mark_interrupted_runs(c, now)).await?;
    if interrupted > 0 {
        tracing::warn!(count = interrupted, "marked interrupted runs as failed");
    }
    let has_data = db.call(|c| db::count_encrypted_items(c)).await? > 0;
    let s2 = secrets.clone();
    let key =
        tokio::task::spawn_blocking(move || load_or_create_storage_key(&s2, has_data)).await??;
    let http = build_http_client(&cfg)?;
    let install_id = db
        .call(|c| match db::get_meta(c, "install_id")? {
            Some(v) => Ok(v),
            None => {
                let v = crate::util::new_id();
                db::set_meta(c, "install_id", &v)?;
                Ok(v)
            }
        })
        .await?;
    let telemetry = Telemetry::new(&cfg, &secrets, http.clone(), install_id)?;

    let port = opts.port.unwrap_or(cfg.server.port);
    let tcp = TcpListener::bind(("127.0.0.1", port))
        .await
        .with_context(|| format!("binding 127.0.0.1:{port}"))?;
    let tcp_addr = tcp.local_addr()?;
    write_private_file_atomic(
        &paths.port_file(),
        format!("{}\n", tcp_addr.port()).as_bytes(),
    )?;
    let unix = if opts.unix_socket.unwrap_or(cfg.server.unix_socket) {
        Some(bind_unix(&paths.socket())?)
    } else {
        None
    };
    let socket_path = unix.as_ref().map(|_| paths.socket());

    let shutdown = CancellationToken::new();
    let core = Arc::new(Core {
        cfg: cfg.clone(),
        paths: paths.clone(),
        db,
        secrets,
        key,
        http,
        telemetry: telemetry.clone(),
        shutdown: shutdown.clone(),
    });
    let runs = RunManager::new(core.clone());
    let state = AppState { core, runs };
    let header_timeout = Duration::from_secs(cfg.server.header_read_timeout_secs);
    let tcp_router = build_router(
        state.clone(),
        Guard {
            token: token.clone(),
            transport: Transport::Tcp {
                port: tcp_addr.port(),
            },
        },
    );
    let mut tasks = vec![tokio::spawn(serve_listener(
        tcp,
        tcp_router,
        shutdown.clone(),
        header_timeout,
    ))];
    if let Some(u) = unix {
        let r = build_router(
            state.clone(),
            Guard {
                token,
                transport: Transport::Unix,
            },
        );
        tasks.push(tokio::spawn(serve_listener(
            u,
            r,
            shutdown.clone(),
            header_timeout,
        )));
    }
    let st = state.clone();
    let sd = shutdown.clone();
    let p2 = paths.clone();
    let sock = socket_path.clone();
    let join = tokio::spawn(async move {
        sd.cancelled().await;
        st.runs.wait_idle(Duration::from_secs(10)).await;
        for t in tasks {
            let _ = t.await;
        }
        if let Some(s) = sock {
            let _ = std::fs::remove_file(s);
        }
        let _ = std::fs::remove_file(p2.port_file());
        tracing::info!("ko-agentd stopped");
    });
    tracing::info!(addr = %tcp_addr, unix = socket_path.is_some(), "ko-agentd listening");
    telemetry.capture("agentd_started", serde_json::json!({}));
    Ok(RunningServer {
        tcp_addr,
        socket_path,
        state,
        shutdown,
        join,
    })
}
