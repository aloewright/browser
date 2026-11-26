/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "brave/components/ko/telemetry/ko_analytics_service.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/containers/contains.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/uuid.h"
#include "brave/components/ko/telemetry/ko_analytics_prefs.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "net/base/load_flags.h"
#include "net/base/net_errors.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/fetch_api.mojom-shared.h"
#include "url/url_constants.h"

namespace ko::analytics {

namespace {

constexpr net::BackoffEntry::Policy kBackoffPolicy = {
    // Number of initial errors to ignore before applying backoff.
    0,
    // Initial delay: 1 minute.
    60 * 1000,
    // Factor by which the delay increases.
    2.0,
    // Fuzzing percentage.
    0.2,
    // Maximum delay: 6 hours.
    6 * 60 * 60 * 1000,
    // Never discard the entry.
    -1,
    // Don't use initial delay unless the last request was an error.
    false,
};

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("ko_analytics", R"(
      semantics {
        sender: "K-O Analytics"
        description:
          "Sends a few allowlisted, anonymous usage events to the user's "
          "self-hosted PostHog server."
        trigger:
          "Browser start, the end of the previous session and the first "
          "daily use of a K-O feature. Batched and retried with backoff."
        data:
          "Event name, an enumerated value (session length bucket or "
          "feature name), browser version, channel, OS, an hour-truncated "
          "timestamp and a random install id rotated every month. Never "
          "URLs, page titles, hostnames or page content."
        destination: OTHER
        destination_other:
          "The PostHog host set by the ko_posthog_host build argument."
      }
      policy {
        cookies_allowed: NO
        setting:
          "Settings > Privacy and security > Data collection > Send "
          "anonymous usage statistics."
        policy_exception_justification:
          "Personal build without enterprise policy support."
      })");

bool IsValidApiKey(std::string_view key) {
  return !key.empty() && key.size() <= 128 &&
         std::ranges::all_of(key, [](char c) {
           return base::IsAsciiAlphaNumeric(c) || c == '_' || c == '-';
         });
}

// Months since year 0, UTC.
int CurrentMonthIndex() {
  base::Time::Exploded exploded;
  base::Time::Now().UTCExplode(&exploded);
  return exploded.year * 12 + (exploded.month - 1);
}

int CurrentUtcDay() {
  return (base::Time::Now() - base::Time::UnixEpoch()).InDays();
}

bool IsRetryable(int net_error, int http_code) {
  if (net_error != net::OK || http_code == 0) {
    return true;
  }
  return http_code == 408 || http_code == 429 || http_code >= 500;
}

}  // namespace

KOAnalyticsService::Config::Config() = default;
KOAnalyticsService::Config::Config(const Config&) = default;
KOAnalyticsService::Config& KOAnalyticsService::Config::operator=(
    const Config&) = default;
KOAnalyticsService::Config::~Config() = default;

// static
std::optional<KOAnalyticsService::Config> KOAnalyticsService::MakeConfig(
    std::string_view host,
    std::string_view api_key,
    CommonProperties common) {
  GURL host_url(host);
  if (!host_url.is_valid() || !host_url.SchemeIs(url::kHttpsScheme) ||
      host_url.host().empty() || host_url.has_username() ||
      host_url.has_password() || host_url.has_query() || host_url.has_ref() ||
      !IsValidApiKey(api_key) || !IsValidCommonProperties(common)) {
    return std::nullopt;
  }
  std::string path(host_url.path());
  if (!path.ends_with('/')) {
    path += '/';
  }
  GURL::Replacements replacements;
  path += "batch/";
  replacements.SetPathStr(path);

  Config config;
  config.endpoint = host_url.ReplaceComponents(replacements);
  config.api_key = std::string(api_key);
  config.common = std::move(common);
  if (!config.endpoint.is_valid() ||
      !config.endpoint.SchemeIs(url::kHttpsScheme)) {
    return std::nullopt;
  }
  return config;
}

KOAnalyticsService::KOAnalyticsService(
    Config config,
    PrefService* local_state,
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory)
    : config_(std::move(config)),
      local_state_(local_state),
      url_loader_factory_(std::move(url_loader_factory)),
      backoff_(&kBackoffPolicy) {
  CHECK(local_state_);
  CHECK(url_loader_factory_);
  CHECK(config_.endpoint.SchemeIs(url::kHttpsScheme));
  pref_change_registrar_.Init(local_state_);
  pref_change_registrar_.Add(
      prefs::kEnabled,
      base::BindRepeating(&KOAnalyticsService::OnEnabledPrefChanged,
                          base::Unretained(this)));
}

KOAnalyticsService::~KOAnalyticsService() = default;

bool KOAnalyticsService::IsEnabled() const {
  return !shut_down_ && local_state_->GetBoolean(prefs::kEnabled);
}

void KOAnalyticsService::OnBrowserStart() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!IsEnabled()) {
    ClearStoredState();
    return;
  }
  SanitizeQueue();

  const base::Time start = local_state_->GetTime(prefs::kSessionStart);
  if (!start.is_null()) {
    base::TimeDelta length =
        local_state_->GetTime(prefs::kSessionLastActive) - start;
    if (length.is_negative()) {
      length = base::TimeDelta();
    }
    Enqueue(kEventSessionLengthBucket,
            base::DictValue().Set(
                "bucket", SessionLengthBucketToString(
                              BucketForSessionLength(length))));
    if (local_state_->GetBoolean(prefs::kSessionCleanExit)) {
      Enqueue(kEventCrashFreeSession, base::DictValue());
    }
  }

  StartSession();
  Enqueue(kEventBrowserStart, base::DictValue());
}

void KOAnalyticsService::OnBrowserShutdown() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (IsEnabled() && !local_state_->GetTime(prefs::kSessionStart).is_null()) {
    local_state_->SetTime(prefs::kSessionLastActive, base::Time::Now());
    local_state_->SetBoolean(prefs::kSessionCleanExit, true);
  }
  shut_down_ = true;
  pref_change_registrar_.RemoveAll();
  StopWork();
}

void KOAnalyticsService::RecordFeatureUsed(Feature feature) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!IsEnabled()) {
    return;
  }
  const std::string_view name = FeatureToString(feature);
  const int today = CurrentUtcDay();
  if (local_state_->GetDict(prefs::kFeatureLastReportedDay).FindInt(name) ==
      today) {
    return;
  }
  ScopedDictPrefUpdate(local_state_, prefs::kFeatureLastReportedDay)
      ->Set(name, today);
  Enqueue(kEventFeatureUsed, base::DictValue().Set("feature", name));
}

void KOAnalyticsService::EnqueueForTesting(std::string_view event,
                                           base::DictValue props) {
  Enqueue(event, std::move(props));
}

size_t KOAnalyticsService::GetQueueSizeForTesting() const {
  return local_state_->GetList(prefs::kQueue).size();
}

void KOAnalyticsService::Enqueue(std::string_view event,
                                 base::DictValue props) {
  if (!IsEnabled()) {
    return;
  }
  std::optional<base::DictValue> scrubbed = ScrubEventProperties(event, props);
  if (!scrubbed) {
    return;
  }
  base::DictValue entry;
  entry.Set(kQueuedEventKey, event);
  entry.Set(kQueuedPropertiesKey, std::move(*scrubbed));
  entry.Set(kQueuedTimestampKey, FormatHourTimestamp(base::Time::Now()));
  entry.Set(kQueuedInstallIdKey, GetOrRotateInstallId());
  entry.Set(kQueuedUuidKey, base::Uuid::GenerateRandomV4().AsLowercaseString());
  {
    ScopedListPrefUpdate update(local_state_, prefs::kQueue);
    update->Append(std::move(entry));
    // Drop the oldest events once the cap is reached.
    while (update->size() > kMaxQueueSize) {
      update->erase(update->begin());
    }
  }
  MaybeScheduleUpload(kBatchDelay);
}

std::string KOAnalyticsService::GetOrRotateInstallId() {
  const int month = CurrentMonthIndex();
  const std::string& current = local_state_->GetString(prefs::kInstallId);
  if (local_state_->GetInteger(prefs::kInstallIdPeriod) == month &&
      IsValidInstallId(current)) {
    return current;
  }
  std::string id = base::HexEncodeLower(base::RandBytesAsVector(16));
  local_state_->SetString(prefs::kInstallId, id);
  local_state_->SetInteger(prefs::kInstallIdPeriod, month);
  return id;
}

void KOAnalyticsService::StartSession() {
  const base::Time now = base::Time::Now();
  local_state_->SetTime(prefs::kSessionStart, now);
  local_state_->SetTime(prefs::kSessionLastActive, now);
  local_state_->SetBoolean(prefs::kSessionCleanExit, false);
  heartbeat_timer_.Start(FROM_HERE, kHeartbeatInterval,
                         base::BindRepeating(&KOAnalyticsService::Heartbeat,
                                             base::Unretained(this)));
}

void KOAnalyticsService::Heartbeat() {
  if (IsEnabled()) {
    local_state_->SetTime(prefs::kSessionLastActive, base::Time::Now());
  }
}

void KOAnalyticsService::SanitizeQueue() {
  // Local state is not trusted input: drop anything that no longer passes the
  // allowlist (e.g. hand-edited or from an older build).
  ScopedListPrefUpdate update(local_state_, prefs::kQueue);
  update->EraseIf([this](const base::Value& value) {
    const base::DictValue* dict = value.GetIfDict();
    return !dict || !BuildPostHogEvent(*dict, config_.common).has_value();
  });
  while (update->size() > kMaxQueueSize) {
    update->erase(update->begin());
  }
}

void KOAnalyticsService::MaybeScheduleUpload(base::TimeDelta delay) {
  if (!IsEnabled() || loader_ || upload_timer_.IsRunning() ||
      local_state_->GetList(prefs::kQueue).empty()) {
    return;
  }
  delay = std::max(delay, backoff_.GetTimeUntilRelease());
  upload_timer_.Start(
      FROM_HERE, delay,
      base::BindOnce(&KOAnalyticsService::Upload, base::Unretained(this)));
}

void KOAnalyticsService::Upload() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!IsEnabled() || loader_) {
    return;
  }
  SanitizeQueue();

  base::ListValue batch;
  std::vector<std::string> uuids;
  for (const base::Value& value : local_state_->GetList(prefs::kQueue)) {
    if (batch.size() >= kMaxBatchSize) {
      break;
    }
    const base::DictValue& queued = value.GetDict();
    std::optional<base::DictValue> event =
        BuildPostHogEvent(queued, config_.common);
    CHECK(event);  // SanitizeQueue() dropped the rest.
    uuids.push_back(*queued.FindString(kQueuedUuidKey));
    batch.Append(std::move(*event));
  }
  if (batch.empty()) {
    return;
  }

  std::optional<std::string> body =
      base::WriteJson(base::DictValue()
                          .Set("api_key", config_.api_key)
                          .Set("historical_migration", false)
                          .Set("batch", std::move(batch)));
  if (!body) {
    return;
  }

  auto request = std::make_unique<network::ResourceRequest>();
  request->url = config_.endpoint;
  request->method = net::HttpRequestHeaders::kPostMethod;
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  // Never follow a redirect away from the configured https endpoint.
  request->redirect_mode = network::mojom::RedirectMode::kError;
  request->load_flags = net::LOAD_DISABLE_CACHE;

  loader_ =
      network::SimpleURLLoader::Create(std::move(request), kTrafficAnnotation);
  loader_->AttachStringForUpload(*body, "application/json");
  loader_->SetAllowHttpErrorResults(true);
  loader_->SetTimeoutDuration(kUploadTimeout);
  loader_->DownloadHeadersOnly(
      url_loader_factory_.get(),
      base::BindOnce(&KOAnalyticsService::OnUploadComplete,
                     weak_factory_.GetWeakPtr(), std::move(uuids)));
}

void KOAnalyticsService::OnUploadComplete(
    std::vector<std::string> uuids,
    scoped_refptr<net::HttpResponseHeaders> headers) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const int net_error = loader_ ? loader_->NetError() : net::ERR_FAILED;
  loader_.reset();
  if (!IsEnabled()) {
    return;
  }
  const int http_code = headers ? headers->response_code() : 0;
  const bool success =
      net_error == net::OK && http_code >= 200 && http_code < 300;
  if (success) {
    backoff_.InformOfRequest(true);
    RemoveFromQueue(uuids);
  } else {
    // Only error codes; never the URL, key or payload.
    VLOG(1) << "K-O analytics upload failed: net=" << net_error
            << " http=" << http_code;
    backoff_.InformOfRequest(false);
    if (!IsRetryable(net_error, http_code)) {
      // The server rejected this batch; retrying won't help.
      RemoveFromQueue(uuids);
    }
  }
  MaybeScheduleUpload(kBatchDelay);
}

void KOAnalyticsService::RemoveFromQueue(
    const std::vector<std::string>& uuids) {
  ScopedListPrefUpdate update(local_state_, prefs::kQueue);
  update->EraseIf([&uuids](const base::Value& value) {
    const base::DictValue* dict = value.GetIfDict();
    const std::string* uuid = dict ? dict->FindString(kQueuedUuidKey) : nullptr;
    return !uuid || base::Contains(uuids, *uuid);
  });
}

void KOAnalyticsService::OnEnabledPrefChanged() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (IsEnabled()) {
    // Fresh, unlinkable install id and session from now on.
    StartSession();
    return;
  }
  StopWork();
  ClearStoredState();
}

void KOAnalyticsService::StopWork() {
  upload_timer_.Stop();
  heartbeat_timer_.Stop();
  loader_.reset();
  weak_factory_.InvalidateWeakPtrs();
}

void KOAnalyticsService::ClearStoredState() {
  local_state_->ClearPref(prefs::kQueue);
  local_state_->ClearPref(prefs::kInstallId);
  local_state_->ClearPref(prefs::kInstallIdPeriod);
  local_state_->ClearPref(prefs::kFeatureLastReportedDay);
  local_state_->ClearPref(prefs::kSessionStart);
  local_state_->ClearPref(prefs::kSessionLastActive);
  local_state_->ClearPref(prefs::kSessionCleanExit);
}

}  // namespace ko::analytics
