/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef BRAVE_BROWSER_KO_TELEMETRY_KO_TELEMETRY_H_
#define BRAVE_BROWSER_KO_TELEMETRY_KO_TELEMETRY_H_

#include "brave/components/ko/telemetry/ko_analytics_event.h"

namespace ko {

// Records an anonymous feature_used event (at most once per feature per UTC
// day). No-op unless analytics is configured at build time and the user left
// it enabled. UI thread only.
void RecordFeatureUsed(analytics::Feature feature);

}  // namespace ko

#endif  // BRAVE_BROWSER_KO_TELEMETRY_KO_TELEMETRY_H_
