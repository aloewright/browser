/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef BRAVE_COMPONENTS_KO_TELEMETRY_KO_ANALYTICS_SERVICE_H_
#define BRAVE_COMPONENTS_KO_TELEMETRY_KO_ANALYTICS_SERVICE_H_

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "brave/components/ko/telemetry/ko_analytics_event.h"
#include "components/prefs/pref_change_registrar.h"
#include "net/base/backoff_entry.h"
#include "url/gurl.h"

class PrefService;

namespace net {
class HttpResponseHeaders;
}

namespace network {
class SharedURLLoaderFactory;
class SimpleURLLoader;
}  // namespace network

namespace ko::analytics {

// Browser-wide, anonymous usage events for the user's self-hosted PostHog.
// Only the allowlisted events in ko_analytics_event.h exist. Events are queued
// in local state (capped, survives offline periods and restarts) and sent in
// batches over https with exponential backoff. Nothing is recorded or sent
// while the kEnabled pref is off; turning it off clears all stored state.
class KOAnalyticsService {
 public:
  struct Config {
    Config();
    Config(const Config&);
    Config& operator=(const Config&);
    ~Config();

    GURL endpoint;  // https://<host>/<path>/batch/
    std::string api_key;
    CommonProperties common;
  };

  // nullopt unless `host` is an https URL without credentials, query or
  // fragment, and `api_key` / `common` are well-formed.
  static std::optional<Config> MakeConfig(std::string_view host,
                                          std::string_view api_key,
                                          CommonProperties common);

  static constexpr size_t kMaxQueueSize = 200;
  static constexpr size_t kMaxBatchSize = 50;
  static constexpr base::TimeDelta kBatchDelay = base::Seconds(60);
  static constexpr base::TimeDelta kHeartbeatInterval = base::Minutes(5);
  static constexpr base::TimeDelta kUploadTimeout = base::Seconds(30);

  KOAnalyticsService(
      Config config,
      PrefService* local_state,
      scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory);
  KOAnalyticsService(const KOAnalyticsService&) = delete;
  KOAnalyticsService& operator=(const KOAnalyticsService&) = delete;
  ~KOAnalyticsService();

  // Reports the previous session (length bucket, clean exit) and starts a
  // new one with a browser_start event.
  void OnBrowserStart();
  // Marks the session as cleanly ended and stops all timers and uploads.
  void OnBrowserShutdown();

  // At most one feature_used per feature per UTC day.
  void RecordFeatureUsed(Feature feature);

  bool IsEnabled() const;

  // Validates `event` and `props` like any other event.
  void EnqueueForTesting(std::string_view event, base::DictValue props);
  size_t GetQueueSizeForTesting() const;

 private:
  void Enqueue(std::string_view event, base::DictValue props);
  std::string GetOrRotateInstallId();
  void StartSession();
  void Heartbeat();
  void SanitizeQueue();
  void MaybeScheduleUpload(base::TimeDelta delay);
  void Upload();
  void OnUploadComplete(std::vector<std::string> uuids,
                        scoped_refptr<net::HttpResponseHeaders> headers);
  void RemoveFromQueue(const std::vector<std::string>& uuids);
  void OnEnabledPrefChanged();
  void StopWork();
  void ClearStoredState();

  const Config config_;
  const raw_ptr<PrefService> local_state_;
  scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory_;
  PrefChangeRegistrar pref_change_registrar_;
  net::BackoffEntry backoff_;
  base::OneShotTimer upload_timer_;
  base::RepeatingTimer heartbeat_timer_;
  std::unique_ptr<network::SimpleURLLoader> loader_;
  bool shut_down_ = false;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<KOAnalyticsService> weak_factory_{this};
};

}  // namespace ko::analytics

#endif  // BRAVE_COMPONENTS_KO_TELEMETRY_KO_ANALYTICS_SERVICE_H_
