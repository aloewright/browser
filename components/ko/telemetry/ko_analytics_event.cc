/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "brave/components/ko/telemetry/ko_analytics_event.h"

#include <algorithm>
#include <array>
#include <utility>

#include "base/containers/contains.h"
#include "base/containers/span.h"
#include "base/notreached.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/uuid.h"

namespace ko::analytics {

namespace {

constexpr auto kFeatureValues = std::to_array<std::string_view>(
    {"workspace", "machines", "assistant", "container"});

constexpr auto kBucketValues = std::to_array<std::string_view>(
    {"lt_5m", "5m_30m", "30m_2h", "2h_8h", "gt_8h"});

constexpr auto kAllowedChannels = std::to_array<std::string_view>(
    {"release", "beta", "dev", "nightly", "developer", "unknown"});

constexpr auto kAllowedOs =
    std::to_array<std::string_view>({"mac", "linux", "windows"});

struct PropertyRule {
  std::string_view key;
  base::span<const std::string_view> values;
};

struct EventRule {
  std::string_view name;
  base::span<const PropertyRule> properties;  // All required.
};

constexpr PropertyRule kSessionLengthRules[] = {{"bucket", kBucketValues}};
constexpr PropertyRule kFeatureUsedRules[] = {{"feature", kFeatureValues}};

constexpr EventRule kEventRules[] = {
    {kEventBrowserStart, {}},
    {kEventSessionLengthBucket, kSessionLengthRules},
    {kEventCrashFreeSession, {}},
    {kEventFeatureUsed, kFeatureUsedRules},
};

const EventRule* FindEventRule(std::string_view event) {
  for (const auto& rule : kEventRules) {
    if (rule.name == event) {
      return &rule;
    }
  }
  return nullptr;
}

bool IsDigits(std::string_view s) {
  return !s.empty() && std::ranges::all_of(s, base::IsAsciiDigit<char>);
}

}  // namespace

CommonProperties::CommonProperties() = default;
CommonProperties::CommonProperties(std::string app_version,
                                   std::string channel,
                                   std::string os)
    : app_version(std::move(app_version)),
      channel(std::move(channel)),
      os(std::move(os)) {}
CommonProperties::CommonProperties(const CommonProperties&) = default;
CommonProperties::CommonProperties(CommonProperties&&) = default;
CommonProperties& CommonProperties::operator=(const CommonProperties&) =
    default;
CommonProperties& CommonProperties::operator=(CommonProperties&&) = default;
CommonProperties::~CommonProperties() = default;

std::string_view FeatureToString(Feature feature) {
  switch (feature) {
    case Feature::kWorkspace:
      return kFeatureValues[0];
    case Feature::kMachines:
      return kFeatureValues[1];
    case Feature::kAssistant:
      return kFeatureValues[2];
    case Feature::kContainer:
      return kFeatureValues[3];
  }
  NOTREACHED();
}

std::string_view SessionLengthBucketToString(SessionLengthBucket bucket) {
  switch (bucket) {
    case SessionLengthBucket::kUnder5Minutes:
      return kBucketValues[0];
    case SessionLengthBucket::k5To30Minutes:
      return kBucketValues[1];
    case SessionLengthBucket::k30MinutesTo2Hours:
      return kBucketValues[2];
    case SessionLengthBucket::k2To8Hours:
      return kBucketValues[3];
    case SessionLengthBucket::kOver8Hours:
      return kBucketValues[4];
  }
  NOTREACHED();
}

SessionLengthBucket BucketForSessionLength(base::TimeDelta length) {
  if (length < base::Minutes(5)) {
    return SessionLengthBucket::kUnder5Minutes;
  }
  if (length < base::Minutes(30)) {
    return SessionLengthBucket::k5To30Minutes;
  }
  if (length < base::Hours(2)) {
    return SessionLengthBucket::k30MinutesTo2Hours;
  }
  if (length < base::Hours(8)) {
    return SessionLengthBucket::k2To8Hours;
  }
  return SessionLengthBucket::kOver8Hours;
}

std::optional<base::DictValue> ScrubEventProperties(
    std::string_view event,
    const base::DictValue& props) {
  const EventRule* rule = FindEventRule(event);
  if (!rule) {
    return std::nullopt;
  }
  base::DictValue scrubbed;
  for (const auto& property : rule->properties) {
    const std::string* value = props.FindString(property.key);
    if (!value || !base::Contains(property.values, *value)) {
      return std::nullopt;
    }
    scrubbed.Set(property.key, *value);
  }
  // Anything not in the rule is dropped.
  return scrubbed;
}

bool IsValidCommonProperties(const CommonProperties& common) {
  if (common.app_version.empty() || common.app_version.size() > 32 ||
      !std::ranges::all_of(common.app_version, [](char c) {
        return base::IsAsciiDigit(c) || c == '.';
      })) {
    return false;
  }
  return base::Contains(kAllowedChannels, common.channel) &&
         base::Contains(kAllowedOs, common.os);
}

bool IsValidInstallId(std::string_view id) {
  return id.size() == 32 && std::ranges::all_of(id, [](char c) {
           return base::IsAsciiDigit(c) || (c >= 'a' && c <= 'f');
         });
}

std::string FormatHourTimestamp(base::Time time) {
  base::Time::Exploded exploded;
  time.UTCExplode(&exploded);
  return base::StringPrintf("%04d-%02d-%02dT%02d:00:00Z", exploded.year,
                            exploded.month, exploded.day_of_month,
                            exploded.hour);
}

bool IsValidHourTimestamp(std::string_view ts) {
  // YYYY-MM-DDTHH:00:00Z
  return ts.size() == 20 && IsDigits(ts.substr(0, 4)) && ts[4] == '-' &&
         IsDigits(ts.substr(5, 2)) && ts[7] == '-' &&
         IsDigits(ts.substr(8, 2)) && ts[10] == 'T' &&
         IsDigits(ts.substr(11, 2)) && ts.substr(13) == ":00:00Z";
}

std::optional<base::DictValue> BuildPostHogEvent(
    const base::DictValue& queued,
    const CommonProperties& common) {
  const std::string* event = queued.FindString(kQueuedEventKey);
  const base::DictValue* props = queued.FindDict(kQueuedPropertiesKey);
  const std::string* ts = queued.FindString(kQueuedTimestampKey);
  const std::string* id = queued.FindString(kQueuedInstallIdKey);
  const std::string* uuid = queued.FindString(kQueuedUuidKey);
  if (!event || !props || !ts || !id || !uuid ||
      !IsValidHourTimestamp(*ts) || !IsValidInstallId(*id) ||
      !base::Uuid::ParseLowercase(*uuid).is_valid() ||
      !IsValidCommonProperties(common)) {
    return std::nullopt;
  }
  std::optional<base::DictValue> properties =
      ScrubEventProperties(*event, *props);
  if (!properties) {
    return std::nullopt;
  }
  properties->Set("distinct_id", *id);
  properties->Set("app_version", common.app_version);
  properties->Set("channel", common.channel);
  properties->Set("os", common.os);
  properties->Set("$lib", "k-o");
  // Anonymous events: no person profiles, no GeoIP lookup.
  properties->Set("$process_person_profile", false);
  properties->Set("$geoip_disable", true);

  return base::DictValue()
      .Set("event", *event)
      .Set("uuid", *uuid)
      .Set("timestamp", *ts)
      .Set("properties", std::move(*properties));
}

}  // namespace ko::analytics
