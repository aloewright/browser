/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "brave/components/ko/telemetry/ko_analytics_event.h"

#include <optional>
#include <string>

#include "base/json/json_writer.h"
#include "base/time/time.h"
#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ko::analytics {

namespace {

constexpr char kInstallId[] = "0123456789abcdef0123456789abcdef";
constexpr char kUuid[] = "8b5e3f5a-1c2d-4e6f-8a9b-0c1d2e3f4a5b";
constexpr char kTimestamp[] = "2026-11-26T06:00:00Z";

CommonProperties Common() {
  return {.app_version = "1.100.58", .channel = "release", .os = "mac"};
}

base::DictValue Queued(std::string_view event, base::DictValue props) {
  return base::DictValue()
      .Set(kQueuedEventKey, event)
      .Set(kQueuedPropertiesKey, std::move(props))
      .Set(kQueuedTimestampKey, kTimestamp)
      .Set(kQueuedInstallIdKey, kInstallId)
      .Set(kQueuedUuidKey, kUuid);
}

}  // namespace

TEST(KOAnalyticsEventTest, UnknownEventsAreDropped) {
  EXPECT_FALSE(ScrubEventProperties("page_view", base::DictValue()));
  EXPECT_FALSE(ScrubEventProperties("$pageview", base::DictValue()));
  EXPECT_FALSE(ScrubEventProperties("", base::DictValue()));
  EXPECT_FALSE(ScrubEventProperties("BROWSER_START", base::DictValue()));
}

TEST(KOAnalyticsEventTest, AllowlistedEventsPass) {
  EXPECT_EQ(*ScrubEventProperties(kEventBrowserStart, base::DictValue()),
            base::DictValue());
  EXPECT_EQ(*ScrubEventProperties(kEventCrashFreeSession, base::DictValue()),
            base::DictValue());
  for (auto feature : {Feature::kWorkspace, Feature::kMachines,
                       Feature::kAssistant, Feature::kContainer}) {
    auto props = base::DictValue().Set("feature", FeatureToString(feature));
    EXPECT_EQ(*ScrubEventProperties(kEventFeatureUsed, props), props);
  }
  auto props = base::DictValue().Set(
      "bucket", SessionLengthBucketToString(SessionLengthBucket::k2To8Hours));
  EXPECT_EQ(*ScrubEventProperties(kEventSessionLengthBucket, props), props);
}

TEST(KOAnalyticsEventTest, UnknownPropertiesAreStripped) {
  auto scrubbed = ScrubEventProperties(
      kEventBrowserStart, base::DictValue()
                              .Set("url", "https://example.com/private")
                              .Set("title", "Inbox (3)")
                              .Set("hostname", "example.com")
                              .Set("$ip", "100.64.0.10"));
  ASSERT_TRUE(scrubbed);
  EXPECT_TRUE(scrubbed->empty());

  scrubbed = ScrubEventProperties(kEventFeatureUsed,
                                  base::DictValue()
                                      .Set("feature", "workspace")
                                      .Set("workspace_name", "secret project")
                                      .Set("container_id", "abc"));
  ASSERT_TRUE(scrubbed);
  EXPECT_EQ(*scrubbed, base::DictValue().Set("feature", "workspace"));
}

TEST(KOAnalyticsEventTest, NonAllowlistedValuesDropTheEvent) {
  EXPECT_FALSE(ScrubEventProperties(
      kEventFeatureUsed,
      base::DictValue().Set("feature", "https://example.com/")));
  EXPECT_FALSE(ScrubEventProperties(
      kEventFeatureUsed, base::DictValue().Set("feature", "Workspace")));
  EXPECT_FALSE(ScrubEventProperties(kEventFeatureUsed,
                                    base::DictValue().Set("feature", 1)));
  EXPECT_FALSE(ScrubEventProperties(
      kEventSessionLengthBucket, base::DictValue().Set("bucket", "3h12m")));
  // Required properties.
  EXPECT_FALSE(ScrubEventProperties(kEventFeatureUsed, base::DictValue()));
  EXPECT_FALSE(
      ScrubEventProperties(kEventSessionLengthBucket, base::DictValue()));
}

TEST(KOAnalyticsEventTest, SessionLengthBuckets) {
  EXPECT_EQ(BucketForSessionLength(base::TimeDelta()),
            SessionLengthBucket::kUnder5Minutes);
  EXPECT_EQ(BucketForSessionLength(base::Minutes(5) - base::Seconds(1)),
            SessionLengthBucket::kUnder5Minutes);
  EXPECT_EQ(BucketForSessionLength(base::Minutes(5)),
            SessionLengthBucket::k5To30Minutes);
  EXPECT_EQ(BucketForSessionLength(base::Minutes(30)),
            SessionLengthBucket::k30MinutesTo2Hours);
  EXPECT_EQ(BucketForSessionLength(base::Hours(2)),
            SessionLengthBucket::k2To8Hours);
  EXPECT_EQ(BucketForSessionLength(base::Hours(8)),
            SessionLengthBucket::kOver8Hours);
  EXPECT_EQ(BucketForSessionLength(base::Days(30)),
            SessionLengthBucket::kOver8Hours);
}

TEST(KOAnalyticsEventTest, TimestampsAreTruncatedToTheHour) {
  base::Time time;
  ASSERT_TRUE(base::Time::FromUTCString("26 Nov 2026 06:42:17", &time));
  EXPECT_EQ(FormatHourTimestamp(time), "2026-11-26T06:00:00Z");
  EXPECT_TRUE(IsValidHourTimestamp("2026-11-26T06:00:00Z"));
  EXPECT_FALSE(IsValidHourTimestamp("2026-11-26T06:42:17Z"));
  EXPECT_FALSE(IsValidHourTimestamp("2026-11-26T06:00:00.123Z"));
  EXPECT_FALSE(IsValidHourTimestamp("https://example.com"));
}

TEST(KOAnalyticsEventTest, InstallIdFormat) {
  EXPECT_TRUE(IsValidInstallId(kInstallId));
  EXPECT_FALSE(IsValidInstallId("0123456789ABCDEF0123456789ABCDEF"));
  EXPECT_FALSE(IsValidInstallId("user@example.com"));
  EXPECT_FALSE(IsValidInstallId(""));
}

TEST(KOAnalyticsEventTest, CommonPropertiesAreValidated) {
  EXPECT_TRUE(IsValidCommonProperties(Common()));
  auto common = Common();
  common.app_version = "1.100.58 (example.com)";
  EXPECT_FALSE(IsValidCommonProperties(common));
  common = Common();
  common.channel = "my-laptop";
  EXPECT_FALSE(IsValidCommonProperties(common));
  common = Common();
  common.os = "macOS 15.1 on host.example.com";
  EXPECT_FALSE(IsValidCommonProperties(common));
}

TEST(KOAnalyticsEventTest, BuildPostHogEvent) {
  auto event = BuildPostHogEvent(
      Queued(kEventFeatureUsed, base::DictValue()
                                    .Set("feature", "machines")
                                    .Set("url", "https://example.com/")),
      Common());
  ASSERT_TRUE(event);
  EXPECT_EQ(*event->FindString("event"), kEventFeatureUsed);
  EXPECT_EQ(*event->FindString("uuid"), kUuid);
  EXPECT_EQ(*event->FindString("timestamp"), kTimestamp);
  const base::DictValue* props = event->FindDict("properties");
  ASSERT_TRUE(props);
  EXPECT_EQ(*props, base::DictValue()
                        .Set("feature", "machines")
                        .Set("distinct_id", kInstallId)
                        .Set("app_version", "1.100.58")
                        .Set("channel", "release")
                        .Set("os", "mac")
                        .Set("$lib", "k-o")
                        .Set("$process_person_profile", false)
                        .Set("$geoip_disable", true));
  EXPECT_EQ(base::WriteJson(*event)->find("example.com"), std::string::npos);
}

TEST(KOAnalyticsEventTest, BuildPostHogEventRejectsTamperedEntries) {
  const auto common = Common();
  EXPECT_FALSE(BuildPostHogEvent(base::DictValue(), common));
  EXPECT_FALSE(BuildPostHogEvent(
      Queued("page_view", base::DictValue()).Set(kQueuedEventKey, "x"),
      common));
  EXPECT_FALSE(BuildPostHogEvent(Queued(kEventBrowserStart, base::DictValue())
                                     .Set(kQueuedInstallIdKey, "a@example.com"),
                                 common));
  EXPECT_FALSE(BuildPostHogEvent(Queued(kEventBrowserStart, base::DictValue())
                                     .Set(kQueuedUuidKey, "not-a-uuid"),
                                 common));
  EXPECT_FALSE(BuildPostHogEvent(Queued(kEventBrowserStart, base::DictValue())
                                     .Set(kQueuedTimestampKey,
                                          "2026-11-26T06:42:17Z"),
                                 common));
  EXPECT_FALSE(BuildPostHogEvent(
      Queued(kEventFeatureUsed,
             base::DictValue().Set("feature", "https://example.com/")),
      common));
  auto bad_common = common;
  bad_common.channel = "example.com";
  EXPECT_FALSE(
      BuildPostHogEvent(Queued(kEventBrowserStart, base::DictValue()),
                        bad_common));
}

}  // namespace ko::analytics
