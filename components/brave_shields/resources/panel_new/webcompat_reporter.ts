// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

import { loadTimeData } from '$web-common/loadTimeData'

// False in debloated builds, which have no webcompat report endpoint. Defaults
// to true where the key is absent (e.g. Storybook).
export function isWebcompatReporterEnabled(): boolean {
  try {
    return loadTimeData.getBoolean('isWebcompatReporterEnabled')
  } catch {
    return true
  }
}
