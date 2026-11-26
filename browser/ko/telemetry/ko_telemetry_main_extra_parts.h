/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef BRAVE_BROWSER_KO_TELEMETRY_KO_TELEMETRY_MAIN_EXTRA_PARTS_H_
#define BRAVE_BROWSER_KO_TELEMETRY_KO_TELEMETRY_MAIN_EXTRA_PARTS_H_

#include <memory>

#include "chrome/browser/chrome_browser_main_extra_parts.h"

namespace ko {

namespace analytics {
class KOAnalyticsService;
}

// Wires K-O's self-hosted crash reporting and analytics into startup and
// shutdown. Both stay off unless their GN args are set.
class KOTelemetryMainExtraParts : public ChromeBrowserMainExtraParts {
 public:
  KOTelemetryMainExtraParts();
  KOTelemetryMainExtraParts(const KOTelemetryMainExtraParts&) = delete;
  KOTelemetryMainExtraParts& operator=(const KOTelemetryMainExtraParts&) =
      delete;
  ~KOTelemetryMainExtraParts() override;

  // ChromeBrowserMainExtraParts:
  void PostCreateThreads() override;
  void PostBrowserStart() override;
  void PostMainMessageLoopRun() override;

 private:
  std::unique_ptr<analytics::KOAnalyticsService> analytics_;
};

}  // namespace ko

#endif  // BRAVE_BROWSER_KO_TELEMETRY_KO_TELEMETRY_MAIN_EXTRA_PARTS_H_
