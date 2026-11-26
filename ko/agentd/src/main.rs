// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

use std::io::{IsTerminal, Read, Write};
use std::path::PathBuf;

use anyhow::{Context, Result, bail};
use clap::{Parser, Subcommand, ValueEnum};
use ko_agentd::auth::Token;
use ko_agentd::config::Config;
use ko_agentd::paths::Paths;
use ko_agentd::secrets::SecretStore;
use ko_agentd::server::{ServeOptions, start};
use zeroize::Zeroizing;

#[derive(Parser)]
#[command(name = "ko-agentd", version, about = "K-O local agent daemon")]
struct Cli {
    /// Data dir (default: platform K-O data dir; env KO_AGENTD_DATA_DIR).
    #[arg(long, global = true)]
    data_dir: Option<PathBuf>,
    #[arg(long, global = true, value_enum, default_value_t = LogFormat::Text)]
    log_format: LogFormat,
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Clone, Copy, ValueEnum)]
enum LogFormat {
    Text,
    Json,
}

#[derive(Subcommand)]
enum Cmd {
    /// Run the daemon (127.0.0.1 HTTP + Unix socket).
    Serve {
        /// TCP port (default from agentd.toml, else 47821).
        #[arg(long)]
        port: Option<u16>,
        #[arg(long)]
        no_unix_socket: bool,
    },
    /// Show or rotate the bearer token.
    Token {
        #[command(subcommand)]
        cmd: TokenCmd,
    },
    /// Manage Keychain secrets.
    Secrets {
        #[command(subcommand)]
        cmd: SecretsCmd,
    },
    /// Check configuration, permissions, secrets and CLIs.
    Doctor,
}

#[derive(Subcommand)]
enum TokenCmd {
    Show,
    /// New token; restart the daemon and reload browser pages afterwards.
    Rotate,
}

#[derive(Subcommand)]
enum SecretsCmd {
    /// Store a secret; the value is read from stdin.
    Set {
        name: String,
    },
    Delete {
        name: String,
    },
    /// Which secrets referenced by agentd.toml are set.
    Status,
}

fn init_logging(fmt: LogFormat) {
    let filter = tracing_subscriber::EnvFilter::try_from_env("KO_AGENTD_LOG")
        .unwrap_or_else(|_| "info,hyper=warn,reqwest=warn,rustls=warn,h2=warn".into());
    let b = tracing_subscriber::fmt()
        .with_env_filter(filter)
        .with_writer(std::io::stderr)
        .with_target(false);
    match fmt {
        LogFormat::Text => b.init(),
        LogFormat::Json => b.json().init(),
    }
}

fn harden_process() {
    // No core dumps: they would contain decrypted secrets.
    let lim = libc::rlimit {
        rlim_cur: 0,
        rlim_max: 0,
    };
    // SAFETY: plain syscall with a valid pointer.
    unsafe {
        libc::setrlimit(libc::RLIMIT_CORE, &lim);
    }
}

struct EchoOff(Option<libc::termios>);

impl EchoOff {
    fn new() -> Self {
        // SAFETY: termios calls on stdin with a zeroed struct.
        unsafe {
            let mut t: libc::termios = std::mem::zeroed();
            if libc::tcgetattr(0, &mut t) != 0 {
                return Self(None);
            }
            let saved = t;
            t.c_lflag &= !libc::ECHO;
            libc::tcsetattr(0, libc::TCSANOW, &t);
            Self(Some(saved))
        }
    }
}

impl Drop for EchoOff {
    fn drop(&mut self) {
        if let Some(t) = self.0 {
            // SAFETY: restores the saved terminal state.
            unsafe {
                libc::tcsetattr(0, libc::TCSANOW, &t);
            }
        }
    }
}

fn read_secret_stdin() -> Result<Zeroizing<String>> {
    let tty = std::io::stdin().is_terminal();
    let _echo = if tty {
        eprint!("Enter value (input hidden), then press Enter: ");
        let _ = std::io::stderr().flush();
        Some(EchoOff::new())
    } else {
        None
    };
    let mut buf = Zeroizing::new(Vec::new());
    if tty {
        let mut line = Zeroizing::new(String::new());
        std::io::stdin().read_line(&mut line)?;
        eprintln!();
        buf.extend_from_slice(line.as_bytes());
    } else {
        std::io::stdin().take(64 * 1024 + 1).read_to_end(&mut buf)?;
        if buf.len() > 64 * 1024 {
            bail!("secret is larger than 64 KiB");
        }
    }
    let s = String::from_utf8(buf.to_vec()).context("secret must be UTF-8")?;
    let s = Zeroizing::new(s);
    let trimmed = s.strip_suffix('\n').unwrap_or(&s);
    let trimmed = trimmed.strip_suffix('\r').unwrap_or(trimmed);
    Ok(Zeroizing::new(trimmed.to_owned()))
}

fn secret_store(paths: &Paths) -> Result<SecretStore> {
    let cfg = Config::load(&paths.config())?;
    SecretStore::open(cfg.secret_backend()?, paths)
}

async fn serve(paths: Paths, port: Option<u16>, no_unix: bool) -> Result<()> {
    let server = start(
        paths,
        ServeOptions {
            port,
            unix_socket: no_unix.then_some(false),
        },
    )
    .await?;
    let mut term = tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate())?;
    tokio::select! {
        _ = tokio::signal::ctrl_c() => {}
        _ = term.recv() => {}
    }
    tracing::info!("shutting down");
    server.shutdown();
    server.wait().await
}

fn main() -> Result<()> {
    harden_process();
    let cli = Cli::parse();
    init_logging(cli.log_format);
    let paths = Paths::resolve(cli.data_dir)?;
    match cli.cmd {
        Cmd::Serve {
            port,
            no_unix_socket,
        } => tokio::runtime::Builder::new_multi_thread()
            .enable_all()
            .build()?
            .block_on(serve(paths, port, no_unix_socket)),
        Cmd::Token { cmd } => {
            let t = match cmd {
                TokenCmd::Show => {
                    paths.ensure()?;
                    Token::load_or_create(&paths)?
                }
                TokenCmd::Rotate => {
                    paths.ensure()?;
                    let t = Token::rotate(&paths)?;
                    eprintln!("token rotated; restart ko-agentd and reload K-O pages");
                    t
                }
            };
            println!("{}", t.as_str());
            Ok(())
        }
        Cmd::Secrets { cmd } => {
            paths.ensure()?;
            let store = secret_store(&paths)?;
            match cmd {
                SecretsCmd::Set { name } => {
                    let v = read_secret_stdin()?;
                    store.set(&name, &v)?;
                    eprintln!("stored {name} in the {} secret store", store.kind());
                }
                SecretsCmd::Delete { name } => {
                    if store.delete(&name)? {
                        eprintln!("deleted {name}");
                    } else {
                        eprintln!("{name} was not set");
                    }
                }
                SecretsCmd::Status => {
                    let cfg = Config::load(&paths.config())?;
                    for (name, why, _) in ko_agentd::doctor::referenced_secrets(&cfg) {
                        let set = store.get(&name)?.is_some();
                        println!("{name}: {} ({why})", if set { "set" } else { "missing" });
                    }
                }
            }
            Ok(())
        }
        Cmd::Doctor => {
            let rt = tokio::runtime::Builder::new_current_thread()
                .enable_all()
                .build()?;
            let report = rt.block_on(ko_agentd::doctor::run(&paths));
            print!("{}", report.text);
            if report.failures > 0 {
                std::process::exit(1);
            }
            Ok(())
        }
    }
}
