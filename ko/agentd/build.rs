// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

// Embeds assets/ko-agent-worker.js when present (empty otherwise).
fn main() {
    let src = std::path::Path::new("assets/ko-agent-worker.js");
    println!("cargo:rerun-if-changed=assets/ko-agent-worker.js");
    let out = std::path::PathBuf::from(std::env::var("OUT_DIR").expect("OUT_DIR"))
        .join("ko-agent-worker.js");
    let data = std::fs::read(src).unwrap_or_default();
    std::fs::write(out, data).expect("writing embedded bundle");
}
