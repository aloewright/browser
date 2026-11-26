/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "brave/browser/ko/telemetry/ko_telemetry_main_extra_parts.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>

#include "base/logging.h"
#include "base/strings/strcat.h"
#include "brave/browser/ko/telemetry/ko_telemetry.h"
#include "brave/common/brave_channel_info.h"
#include "brave/components/ko/telemetry/analytics_buildflags.h"
#include "brave/components/ko/telemetry/crash_buildflags.h"
#include "brave/components/ko/telemetry/ko_analytics_service.h"
#include "brave/components/version_info/version_info.h"
#include "build/build_config.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/metrics/metrics_reporting_state.h"
#include "chrome/common/crash_keys.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"

namespace ko {

namespace {

analytics::KOAnalyticsService* g_analytics = nullptr;

#if BUILDFLAG(KO_CRASH_REPORTING_CONFIGURED) || \
    BUILDFLAG(KO_ANALYTICS_CONFIGURED)
std::string GetChannel() {
  static constexpr auto kKnown = std::to_array<std::string_view>(
      {"release", "beta", "dev", "nightly", "developer"});
  std::string channel = brave::GetChannelName();
  return std::ranges::contains(kKnown, channel) ? channel : "unknown";
}
#endif

#if BUILDFLAG(KO_ANALYTICS_CONFIGURED)
analytics::CommonProperties GetCommonProperties() {
  analytics::CommonProperties common;
  common.app_version =
      version_info::GetBraveVersionWithoutChromiumMajorVersion();
  common.channel = GetChannel();
#if BUILDFLAG(IS_MAC)
  common.os = "mac";
#elif BUILDFLAG(IS_WIN)
  common.os = "windows";
#else
  common.os = "linux";
#endif
  return common;
}
#endif

}  // namespace

void RecordFeatureUsed(analytics::Feature feature) {
  if (g_analytics) {
    g_analytics->RecordFeatureUsed(feature);
  }
}

KOTelemetryMainExtraParts::KOTelemetryMainExtraParts() = default;

KOTelemetryMainExtraParts::~KOTelemetryMainExtraParts() = default;

void KOTelemetryMainExtraParts::PostCreateThreads() {
#if BUILDFLAG(KO_CRASH_REPORTING_CONFIGURED)
  // Product annotations for the crash server (Sentry/GlitchTip form fields).
  // Only version and channel: no URLs, paths or user data.
  crash_keys::AllocateCrashKeyInBrowserAndChildren(
      "sentry[release]",
      base::StrCat(
          {"k-o@",
           version_info::GetBraveVersionWithoutChromiumMajorVersion()}));
  crash_keys::AllocateCrashKeyInBrowserAndChildren("sentry[environment]",
                                                   GetChannel());
  // Crashpad reads consent from a file that only the settings toggle writes;
  // sync it with the pref, which defaults on when an endpoint is configured.
  metrics::ApplyMetricsReportingPolicy();
#endif
}

void KOTelemetryMainExtraParts::PostBrowserStart() {
#if BUILDFLAG(KO_ANALYTICS_CONFIGURED)
  auto config = analytics::KOAnalyticsService::MakeConfig(
      BUILDFLAG(KO_POSTHOG_HOST), BUILDFLAG(KO_POSTHOG_PROJECT_KEY),
      GetCommonProperties());
  if (!config) {
    // Fail closed. Don't log the values.
    LOG(ERROR) << "K-O analytics off: invalid ko_posthog_host/project key";
    return;
  }
  analytics_ = std::make_unique<analytics::KOAnalyticsService>(
      std::move(*config), g_browser_process->local_state(),
      g_browser_process->shared_url_loader_factory());
  g_analytics = analytics_.get();
  analytics_->OnBrowserStart();
#endif
}

void KOTelemetryMainExtraParts::PostMainMessageLoopRun() {
  if (!analytics_) {
    return;
  }
  // Before local state is committed in BrowserProcessImpl::StartTearDown().
  analytics_->OnBrowserShutdown();
  g_analytics = nullptr;
  analytics_.reset();
}

}  // namespace ko
