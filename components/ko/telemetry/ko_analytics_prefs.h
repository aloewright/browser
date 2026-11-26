/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef BRAVE_COMPONENTS_KO_TELEMETRY_KO_ANALYTICS_PREFS_H_
#define BRAVE_COMPONENTS_KO_TELEMETRY_KO_ANALYTICS_PREFS_H_

class PrefRegistrySimple;

// Local state prefs (browser-wide, not per profile).
namespace ko::analytics::prefs {

// User toggle. Defaults to true only when ko_posthog_host and
// ko_posthog_project_key are both set at build time.
inline constexpr char kEnabled[] = "ko.analytics.enabled";
// Random install id, rotated every calendar month (UTC).
inline constexpr char kInstallId[] = "ko.analytics.install_id";
inline constexpr char kInstallIdPeriod[] = "ko.analytics.install_id_period";
// Offline queue of allowlisted events, capped.
inline constexpr char kQueue[] = "ko.analytics.queue";
// Feature name -> UTC day it was last reported (one feature_used per day).
inline constexpr char kFeatureLastReportedDay[] =
    "ko.analytics.feature_last_reported_day";
// Previous session, for session_length_bucket and crash_free_session.
inline constexpr char kSessionStart[] = "ko.analytics.session.start";
inline constexpr char kSessionLastActive[] = "ko.analytics.session.last_active";
inline constexpr char kSessionCleanExit[] = "ko.analytics.session.clean_exit";

}  // namespace ko::analytics::prefs

namespace ko::analytics {
void RegisterLocalStatePrefs(PrefRegistrySimple* registry);
}  // namespace ko::analytics

#endif  // BRAVE_COMPONENTS_KO_TELEMETRY_KO_ANALYTICS_PREFS_H_
