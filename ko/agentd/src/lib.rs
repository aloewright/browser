// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

//! ko-agentd: the K-O local agent daemon.

pub mod api;
pub mod auth;
pub mod cloudflare;
pub mod config;
pub mod crypto;
pub mod db;
pub mod doctor;
pub mod error;
pub mod mcp;
pub mod paths;
pub mod providers;
pub mod runs;
pub mod secrets;
pub mod server;
pub mod telemetry;
pub mod util;

pub const VERSION: &str = env!("CARGO_PKG_VERSION");
pub const DEFAULT_PORT: u16 = 47821;
