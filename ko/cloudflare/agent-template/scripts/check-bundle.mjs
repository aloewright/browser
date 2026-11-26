// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

// Sanity-checks dist/index.js (the module ko-agentd uploads).
import { readFileSync, statSync } from 'node:fs'

const path = new URL('../dist/index.js', import.meta.url)
const src = readFileSync(path, 'utf8')
const size = statSync(path).size
for (const needle of ['KoAgent', 'cf-access-jwt-assertion', 'export']) {
  if (!src.includes(needle)) {
    console.error(`bundle is missing ${needle}`)
    process.exit(1)
  }
}
console.log(`dist/index.js: ${(size / 1024).toFixed(1)} KiB`)
