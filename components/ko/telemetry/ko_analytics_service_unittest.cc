/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "brave/components/ko/telemetry/ko_analytics_service.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "base/values.h"
#include "brave/components/ko/telemetry/ko_analytics_prefs.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "components/prefs/testing_pref_service.h"
#include "net/http/http_status_code.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/public/mojom/fetch_api.mojom-shared.h"
#include "services/network/test/test_url_loader_factory.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ko::analytics {

namespace {

constexpr char kHost[] = "https://posthog.example.com";
constexpr char kEndpoint[] = "https://posthog.example.com/batch/";
constexpr char kKey[] = "phc_exampleKey123";

CommonProperties Common() {
  return CommonProperties("1.100.58", "release", "linux");
}

}  // namespace

class KOAnalyticsServiceTest : public testing::Test {
 public:
  KOAnalyticsServiceTest() {
    RegisterLocalStatePrefs(local_state_.registry());
    local_state_.SetBoolean(prefs::kEnabled, true);
    test_url_loader_factory_.SetInterceptor(
        base::BindRepeating(&KOAnalyticsServiceTest::OnRequest,
                            base::Unretained(this)));
  }

  void CreateService() {
    auto config = KOAnalyticsService::MakeConfig(kHost, kKey, Common());
    ASSERT_TRUE(config);
    service_ = std::make_unique<KOAnalyticsService>(
        *config, &local_state_,
        base::MakeRefCounted<network::WeakWrapperSharedURLLoaderFactory>(
            &test_url_loader_factory_));
  }

  // Runs past the batch delay so a pending upload is sent.
  void FlushUploads() {
    task_environment_.FastForwardBy(KOAnalyticsService::kBatchDelay +
                                    base::Seconds(1));
  }

  std::vector<std::string> SentEventNames(size_t request) {
    std::vector<std::string> names;
    const base::DictValue& body = bodies_.at(request);
    for (const auto& event : *body.FindList("batch")) {
      names.push_back(*event.GetDict().FindString("event"));
    }
    return names;
  }

  void RespondWith(net::HttpStatusCode code) {
    ASSERT_EQ(test_url_loader_factory_.NumPending(), 1);
    test_url_loader_factory_.AddResponse(kEndpoint, "{}", code);
    task_environment_.RunUntilIdle();
  }

 protected:
  void OnRequest(const network::ResourceRequest& request) {
    EXPECT_EQ(request.url, GURL(kEndpoint));
    EXPECT_EQ(request.method, "POST");
    EXPECT_EQ(request.credentials_mode,
              network::mojom::CredentialsMode::kOmit);
    EXPECT_EQ(request.redirect_mode, network::mojom::RedirectMode::kError);
    ASSERT_TRUE(request.request_body);
    ASSERT_EQ(request.request_body->elements()->size(), 1u);
    const auto& element = request.request_body->elements()->at(0);
    std::string body(element.As<network::DataElementBytes>().AsStringView());
    raw_bodies_.push_back(body);
    auto dict = base::JSONReader::ReadDict(body, base::JSON_PARSE_RFC);
    ASSERT_TRUE(dict);
    EXPECT_EQ(*dict->FindString("api_key"), kKey);
    bodies_.push_back(std::move(*dict));
  }

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  TestingPrefServiceSimple local_state_;
  network::TestURLLoaderFactory test_url_loader_factory_;
  std::unique_ptr<KOAnalyticsService> service_;
  std::vector<base::DictValue> bodies_;
  std::vector<std::string> raw_bodies_;
};

TEST_F(KOAnalyticsServiceTest, MakeConfigRequiresHttpsOrigin) {
  EXPECT_FALSE(KOAnalyticsService::MakeConfig("http://posthog.example.com",
                                              kKey, Common()));
  EXPECT_FALSE(KOAnalyticsService::MakeConfig(
      "https://user:pass@posthog.example.com", kKey, Common()));
  EXPECT_FALSE(KOAnalyticsService::MakeConfig(
      "https://posthog.example.com/?a=b", kKey, Common()));
  EXPECT_FALSE(KOAnalyticsService::MakeConfig("", kKey, Common()));
  EXPECT_FALSE(KOAnalyticsService::MakeConfig(kHost, "", Common()));
  EXPECT_FALSE(KOAnalyticsService::MakeConfig(kHost, "key with spaces",
                                              Common()));

  auto config = KOAnalyticsService::MakeConfig(kHost, kKey, Common());
  ASSERT_TRUE(config);
  EXPECT_EQ(config->endpoint, GURL(kEndpoint));
  config = KOAnalyticsService::MakeConfig("https://example.com/ingest", kKey,
                                          Common());
  ASSERT_TRUE(config);
  EXPECT_EQ(config->endpoint, GURL("https://example.com/ingest/batch/"));
}

TEST_F(KOAnalyticsServiceTest, DisabledRecordsAndSendsNothing) {
  local_state_.SetBoolean(prefs::kEnabled, false);
  CreateService();
  service_->OnBrowserStart();
  service_->RecordFeatureUsed(Feature::kWorkspace);
  FlushUploads();
  EXPECT_EQ(service_->GetQueueSizeForTesting(), 0u);
  EXPECT_EQ(test_url_loader_factory_.NumPending(), 0);
  EXPECT_TRUE(local_state_.GetString(prefs::kInstallId).empty());
  EXPECT_TRUE(local_state_.GetTime(prefs::kSessionStart).is_null());
}

TEST_F(KOAnalyticsServiceTest, BatchesAndUploadsAfterDelay) {
  CreateService();
  service_->OnBrowserStart();
  service_->RecordFeatureUsed(Feature::kMachines);
  EXPECT_EQ(service_->GetQueueSizeForTesting(), 2u);
  EXPECT_EQ(test_url_loader_factory_.NumPending(), 0);

  FlushUploads();
  ASSERT_EQ(bodies_.size(), 1u);
  EXPECT_EQ(SentEventNames(0),
            (std::vector<std::string>{"browser_start", "feature_used"}));
  RespondWith(net::HTTP_OK);
  EXPECT_EQ(service_->GetQueueSizeForTesting(), 0u);

  const std::string& id = local_state_.GetString(prefs::kInstallId);
  EXPECT_EQ(id.size(), 32u);
  for (const auto& event : *bodies_[0].FindList("batch")) {
    EXPECT_EQ(*event.GetDict().FindStringByDottedPath(
                  "properties.distinct_id"),
              id);
  }
}

TEST_F(KOAnalyticsServiceTest, PreviousSessionIsReported) {
  // A previous session of 45 minutes that exited cleanly.
  const base::Time now = base::Time::Now();
  local_state_.SetTime(prefs::kSessionStart, now - base::Hours(2));
  local_state_.SetTime(prefs::kSessionLastActive,
                       now - base::Hours(2) + base::Minutes(45));
  local_state_.SetBoolean(prefs::kSessionCleanExit, true);

  CreateService();
  service_->OnBrowserStart();
  FlushUploads();
  ASSERT_EQ(bodies_.size(), 1u);
  EXPECT_EQ(SentEventNames(0),
            (std::vector<std::string>{"session_length_bucket",
                                      "crash_free_session", "browser_start"}));
  EXPECT_EQ(*bodies_[0].FindList("batch")->front().GetDict()
                 .FindStringByDottedPath("properties.bucket"),
            "30m_2h");
  EXPECT_FALSE(local_state_.GetBoolean(prefs::kSessionCleanExit));

  service_->OnBrowserShutdown();
  EXPECT_TRUE(local_state_.GetBoolean(prefs::kSessionCleanExit));
}

TEST_F(KOAnalyticsServiceTest, CrashedSessionIsNotCrashFree) {
  const base::Time now = base::Time::Now();
  local_state_.SetTime(prefs::kSessionStart, now - base::Minutes(10));
  local_state_.SetTime(prefs::kSessionLastActive, now - base::Minutes(8));
  local_state_.SetBoolean(prefs::kSessionCleanExit, false);

  CreateService();
  service_->OnBrowserStart();
  FlushUploads();
  ASSERT_EQ(bodies_.size(), 1u);
  EXPECT_EQ(SentEventNames(0),
            (std::vector<std::string>{"session_length_bucket",
                                      "browser_start"}));
}

TEST_F(KOAnalyticsServiceTest, RetriesWithBackoffAndKeepsQueue) {
  CreateService();
  service_->OnBrowserStart();
  FlushUploads();
  ASSERT_EQ(bodies_.size(), 1u);
  RespondWith(net::HTTP_SERVICE_UNAVAILABLE);
  EXPECT_EQ(service_->GetQueueSizeForTesting(), 1u);

  // The retry waits for the batch delay / backoff (about a minute).
  task_environment_.FastForwardBy(base::Seconds(30));
  EXPECT_EQ(bodies_.size(), 1u);
  task_environment_.FastForwardBy(base::Seconds(31));
  ASSERT_EQ(bodies_.size(), 2u);
  RespondWith(net::HTTP_OK);
  EXPECT_EQ(service_->GetQueueSizeForTesting(), 0u);
}

TEST_F(KOAnalyticsServiceTest, RejectedBatchIsDropped) {
  CreateService();
  service_->OnBrowserStart();
  FlushUploads();
  RespondWith(net::HTTP_BAD_REQUEST);
  EXPECT_EQ(service_->GetQueueSizeForTesting(), 0u);
}

TEST_F(KOAnalyticsServiceTest, QueueIsCappedAndBatched) {
  CreateService();
  for (size_t i = 0; i < KOAnalyticsService::kMaxQueueSize + 25; ++i) {
    service_->EnqueueForTesting(kEventBrowserStart, base::DictValue());
  }
  EXPECT_EQ(service_->GetQueueSizeForTesting(),
            KOAnalyticsService::kMaxQueueSize);
  FlushUploads();
  ASSERT_EQ(bodies_.size(), 1u);
  EXPECT_EQ(bodies_[0].FindList("batch")->size(),
            KOAnalyticsService::kMaxBatchSize);
  RespondWith(net::HTTP_OK);
  EXPECT_EQ(service_->GetQueueSizeForTesting(),
            KOAnalyticsService::kMaxQueueSize -
                KOAnalyticsService::kMaxBatchSize);
}

TEST_F(KOAnalyticsServiceTest, NonAllowlistedDataNeverLeaves) {
  CreateService();
  service_->EnqueueForTesting("page_view",
                              base::DictValue().Set("url", "https://a.test"));
  service_->EnqueueForTesting(
      kEventFeatureUsed,
      base::DictValue().Set("feature", "https://example.com/inbox"));
  service_->EnqueueForTesting(kEventFeatureUsed,
                              base::DictValue()
                                  .Set("feature", "container")
                                  .Set("title", "Inbox (3)")
                                  .Set("host", "example.com"));
  EXPECT_EQ(service_->GetQueueSizeForTesting(), 1u);

  // A hand-edited queue entry in local state is dropped before upload.
  {
    ScopedListPrefUpdate update(&local_state_, prefs::kQueue);
    update->Append(base::DictValue()
                       .Set("event", "feature_used")
                       .Set("props", base::DictValue().Set(
                                         "feature", "https://example.com/"))
                       .Set("ts", "2026-11-26T06:00:00Z")
                       .Set("id", "0123456789abcdef0123456789abcdef")
                       .Set("uuid", "8b5e3f5a-1c2d-4e6f-8a9b-0c1d2e3f4a5b"));
  }

  FlushUploads();
  ASSERT_EQ(raw_bodies_.size(), 1u);
  EXPECT_EQ(SentEventNames(0), (std::vector<std::string>{"feature_used"}));
  EXPECT_EQ(raw_bodies_[0].find("example.com"), std::string::npos);
  EXPECT_EQ(raw_bodies_[0].find("a.test"), std::string::npos);
  EXPECT_EQ(raw_bodies_[0].find("Inbox"), std::string::npos);
}

TEST_F(KOAnalyticsServiceTest, FeatureUsedOncePerDay) {
  CreateService();
  service_->RecordFeatureUsed(Feature::kAssistant);
  service_->RecordFeatureUsed(Feature::kAssistant);
  service_->RecordFeatureUsed(Feature::kContainer);
  EXPECT_EQ(service_->GetQueueSizeForTesting(), 2u);
  task_environment_.FastForwardBy(base::Days(1));
  service_->RecordFeatureUsed(Feature::kAssistant);
  // The first two were uploaded (no response yet), so still queued.
  EXPECT_EQ(service_->GetQueueSizeForTesting(), 3u);
}

TEST_F(KOAnalyticsServiceTest, InstallIdRotatesMonthly) {
  CreateService();
  service_->RecordFeatureUsed(Feature::kWorkspace);
  const std::string first = local_state_.GetString(prefs::kInstallId);
  ASSERT_FALSE(first.empty());

  // Pretend the id was minted last month.
  local_state_.SetInteger(prefs::kInstallIdPeriod,
                          local_state_.GetInteger(prefs::kInstallIdPeriod) -
                              1);
  service_->RecordFeatureUsed(Feature::kMachines);
  const std::string second = local_state_.GetString(prefs::kInstallId);
  EXPECT_EQ(second.size(), 32u);
  EXPECT_NE(first, second);
}

TEST_F(KOAnalyticsServiceTest, DisablingClearsEverything) {
  CreateService();
  service_->OnBrowserStart();
  service_->RecordFeatureUsed(Feature::kWorkspace);
  ASSERT_GT(service_->GetQueueSizeForTesting(), 0u);

  local_state_.SetBoolean(prefs::kEnabled, false);
  EXPECT_EQ(service_->GetQueueSizeForTesting(), 0u);
  EXPECT_TRUE(local_state_.GetString(prefs::kInstallId).empty());
  EXPECT_TRUE(local_state_.GetTime(prefs::kSessionStart).is_null());
  FlushUploads();
  EXPECT_EQ(test_url_loader_factory_.NumPending(), 0);
  EXPECT_TRUE(bodies_.empty());
}

TEST_F(KOAnalyticsServiceTest, ShutdownStopsUploads) {
  CreateService();
  service_->OnBrowserStart();
  service_->OnBrowserShutdown();
  FlushUploads();
  EXPECT_TRUE(bodies_.empty());
  // Queued events survive for the next session.
  EXPECT_EQ(service_->GetQueueSizeForTesting(), 1u);
}

}  // namespace ko::analytics
