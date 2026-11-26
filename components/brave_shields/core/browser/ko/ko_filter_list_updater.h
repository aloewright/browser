// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef BRAVE_COMPONENTS_BRAVE_SHIELDS_CORE_BROWSER_KO_KO_FILTER_LIST_UPDATER_H_
#define BRAVE_COMPONENTS_BRAVE_SHIELDS_CORE_BROWSER_KO_KO_FILTER_LIST_UPDATER_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "brave/components/brave_shields/core/browser/ko/ko_filter_list_manifest.h"
#include "url/gurl.h"

class PrefRegistrySimple;
class PrefService;

namespace network {
class SharedURLLoaderFactory;
class SimpleURLLoader;
}  // namespace network

namespace brave_shields {

class KoFilterListSource;

// Highest channel manifest version ever accepted (anti-rollback), as a
// decimal string in local state.
inline constexpr char kKoFilterListsAcceptedVersion[] =
    "brave.ko.shields.filter_lists_accepted_version";

// Pulls the signed K-O filter list channel (GN arg ko_filter_lists_url) over
// HTTPS. A snapshot is used only if its manifest signature, every file's
// size and SHA-256 and the version checks pass; otherwise the current set
// stays in effect.
class KoFilterListUpdater {
 public:
  KoFilterListUpdater(KoFilterListSource* source,
                      scoped_refptr<network::SharedURLLoaderFactory> factory,
                      PrefService* local_state,
                      std::string_view base_url,
                      base::RepeatingClosure on_installed);
  KoFilterListUpdater(const KoFilterListUpdater&) = delete;
  KoFilterListUpdater& operator=(const KoFilterListUpdater&) = delete;
  ~KoFilterListUpdater();

  static void RegisterLocalStatePrefs(PrefRegistrySimple* registry);

  // Returns the channel base URL if `url` is an https URL without query,
  // fragment or credentials; otherwise an invalid GURL.
  static GURL ValidateBaseUrl(std::string_view url);

  // Whether a verified manifest `version` may replace the current set.
  static bool ShouldAccept(int64_t version,
                           int64_t embedded_version,
                           int64_t accepted_version,
                           bool remote_active,
                           int64_t active_version);

  // Schedules the first check shortly after startup, then periodically.
  void Start();
  void CheckNow();

 private:
  struct Pending {
    Pending();
    ~Pending();
    std::string manifest_json;
    std::string signature;
    KoFilterListManifest manifest;
    size_t next = 0;
    std::vector<std::pair<std::string, std::string>> files;
  };

  void ScheduleCheck(base::TimeDelta delay);
  void Fetch(const std::string& file,
             size_t max_size,
             base::OnceCallback<void(std::optional<std::string>)> callback);
  void OnManifest(std::optional<std::string> body);
  void OnSignature(std::optional<std::string> body);
  void FetchNextList();
  void OnList(std::optional<std::string> body);
  void OnInstalled(bool success);
  void Finish(bool success, const char* reason);

  raw_ptr<KoFilterListSource> source_;
  scoped_refptr<network::SharedURLLoaderFactory> factory_;
  raw_ptr<PrefService> local_state_;
  const GURL base_url_;
  base::RepeatingClosure on_installed_;

  std::unique_ptr<network::SimpleURLLoader> loader_;
  std::unique_ptr<Pending> pending_;
  base::OneShotTimer timer_;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<KoFilterListUpdater> weak_factory_{this};
};

}  // namespace brave_shields

#endif  // BRAVE_COMPONENTS_BRAVE_SHIELDS_CORE_BROWSER_KO_KO_FILTER_LIST_UPDATER_H_
