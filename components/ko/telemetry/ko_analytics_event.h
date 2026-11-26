/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef BRAVE_COMPONENTS_KO_TELEMETRY_KO_ANALYTICS_EVENT_H_
#define BRAVE_COMPONENTS_KO_TELEMETRY_KO_ANALYTICS_EVENT_H_

#include <optional>
#include <string>
#include <string_view>

#include "base/time/time.h"
#include "base/values.h"

// The only data K-O analytics can send. Everything here is an allowlist:
// unknown events, properties or values are dropped, never forwarded.
namespace ko::analytics {

enum class Feature {
  kWorkspace,
  kMachines,
  kAssistant,
  kContainer,
};

enum class SessionLengthBucket {
  kUnder5Minutes,
  k5To30Minutes,
  k30MinutesTo2Hours,
  k2To8Hours,
  kOver8Hours,
};

inline constexpr char kEventBrowserStart[] = "browser_start";
inline constexpr char kEventSessionLengthBucket[] = "session_length_bucket";
inline constexpr char kEventCrashFreeSession[] = "crash_free_session";
inline constexpr char kEventFeatureUsed[] = "feature_used";

std::string_view FeatureToString(Feature feature);
std::string_view SessionLengthBucketToString(SessionLengthBucket bucket);
SessionLengthBucket BucketForSessionLength(base::TimeDelta length);

// Build-wide properties added to every event. Validated, never free text.
struct CommonProperties {
  CommonProperties();
  CommonProperties(std::string app_version,
                   std::string channel,
                   std::string os);
  CommonProperties(const CommonProperties&);
  CommonProperties(CommonProperties&&);
  CommonProperties& operator=(const CommonProperties&);
  CommonProperties& operator=(CommonProperties&&);
  ~CommonProperties();

  std::string app_version;  // Digits and dots only, e.g. "1.100.58".
  std::string channel;      // One of kAllowedChannels.
  std::string os;           // "mac", "linux" or "windows".
};

// Returns `props` if `event` and every property and value are allowlisted,
// otherwise nullopt (fail closed: the event is dropped).
std::optional<base::DictValue> ScrubEventProperties(
    std::string_view event,
    const base::DictValue& props);

bool IsValidCommonProperties(const CommonProperties& common);

// `id` must be 32 lowercase hex chars.
bool IsValidInstallId(std::string_view id);

// UTC, truncated to the hour, e.g. "2026-11-26T06:00:00Z".
std::string FormatHourTimestamp(base::Time time);
bool IsValidHourTimestamp(std::string_view timestamp);

// A queued event as persisted in local state. Re-validated before upload.
// Returns the PostHog batch entry, or nullopt if anything is off.
std::optional<base::DictValue> BuildPostHogEvent(
    const base::DictValue& queued,
    const CommonProperties& common);

// Keys of a queued event.
inline constexpr char kQueuedEventKey[] = "event";
inline constexpr char kQueuedPropertiesKey[] = "props";
inline constexpr char kQueuedTimestampKey[] = "ts";
inline constexpr char kQueuedInstallIdKey[] = "id";
inline constexpr char kQueuedUuidKey[] = "uuid";

}  // namespace ko::analytics

#endif  // BRAVE_COMPONENTS_KO_TELEMETRY_KO_ANALYTICS_EVENT_H_
