// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/brave_shields/core/browser/ko/ko_filter_list_updater.h"

#include <utility>

#include "base/check.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "brave/components/brave_shields/core/browser/ko/ko_filter_list_source.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"
#include "net/base/load_flags.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/fetch_api.mojom-shared.h"
#include "url/url_constants.h"

namespace brave_shields {

namespace {

constexpr char kManifestFile[] = "manifest.json";
constexpr char kSignatureFile[] = "manifest.json.sig";
constexpr size_t kMaxSignatureBytes = 1024;
constexpr base::TimeDelta kInitialDelay = base::Minutes(2);
constexpr base::TimeDelta kCheckInterval = base::Hours(6);
constexpr base::TimeDelta kRetryInterval = base::Hours(1);
constexpr base::TimeDelta kRequestTimeout = base::Minutes(5);

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("ko_filter_lists_update", R"(
      semantics {
        sender: "K-O filter list updater"
        description:
          "Downloads the signed K-O ad and tracker filter list snapshot from "
          "the HTTPS location configured at build time."
        trigger: "Shortly after startup, then every few hours."
        data: "None. No cookies or credentials are sent."
        destination: OTHER
        destination_other: "The K-O filter list channel set at build time."
      }
      policy {
        cookies_allowed: NO
        setting: "Disabled unless the ko_filter_lists_url GN arg is set."
        policy_exception_justification: "Not implemented."
      })");

int64_t GetAcceptedVersion(PrefService* local_state) {
  int64_t version = 0;
  if (!base::StringToInt64(
          local_state->GetString(kKoFilterListsAcceptedVersion), &version)) {
    return 0;
  }
  return version;
}

// Writes the snapshot next to `dir`, then swaps it in. Runs on the source's
// file sequence so readers never see a partial set.
bool InstallRemoteSet(base::FilePath dir,
                      std::string manifest_json,
                      std::string signature,
                      std::vector<std::pair<std::string, std::string>> files) {
  const base::FilePath staging = dir.AddExtensionASCII("new");
  const base::FilePath previous = dir.AddExtensionASCII("old");
  base::DeletePathRecursively(staging);
  base::DeletePathRecursively(previous);
  if (!base::CreateDirectory(staging)) {
    return false;
  }
  for (const auto& [name, data] : files) {
    if (!base::WriteFile(staging.AppendASCII(name), data)) {
      return false;
    }
  }
  if (!base::WriteFile(staging.AppendASCII(kSignatureFile), signature) ||
      !base::WriteFile(staging.AppendASCII(kManifestFile), manifest_json)) {
    return false;
  }
  if (base::PathExists(dir) && !base::Move(dir, previous)) {
    return false;
  }
  if (!base::Move(staging, dir)) {
    base::Move(previous, dir);
    return false;
  }
  base::DeletePathRecursively(previous);
  return true;
}

}  // namespace

KoFilterListUpdater::Pending::Pending() = default;
KoFilterListUpdater::Pending::~Pending() = default;

KoFilterListUpdater::KoFilterListUpdater(
    KoFilterListSource* source,
    scoped_refptr<network::SharedURLLoaderFactory> factory,
    PrefService* local_state,
    std::string_view base_url,
    base::RepeatingClosure on_installed)
    : source_(source),
      factory_(std::move(factory)),
      local_state_(local_state),
      base_url_(ValidateBaseUrl(base_url)),
      on_installed_(std::move(on_installed)) {
  CHECK(source_);
  CHECK(local_state_);
}

KoFilterListUpdater::~KoFilterListUpdater() = default;

// static
void KoFilterListUpdater::RegisterLocalStatePrefs(
    PrefRegistrySimple* registry) {
  registry->RegisterStringPref(kKoFilterListsAcceptedVersion, "0");
}

// static
GURL KoFilterListUpdater::ValidateBaseUrl(std::string_view url) {
  GURL gurl(url);
  if (!gurl.is_valid() || !gurl.SchemeIs(url::kHttpsScheme) ||
      gurl.has_query() || gurl.has_ref() || gurl.has_username() ||
      gurl.has_password()) {
    return GURL();
  }
  // Normalise to a directory URL so relative file names resolve below it.
  std::string spec = gurl.spec();
  if (!spec.ends_with('/')) {
    spec += '/';
  }
  return GURL(spec);
}

// static
bool KoFilterListUpdater::ShouldAccept(int64_t version,
                                       int64_t embedded_version,
                                       int64_t accepted_version,
                                       bool remote_active,
                                       int64_t active_version) {
  if (version <= embedded_version) {
    return false;  // The browser already ships something at least as new.
  }
  if (remote_active && version <= active_version) {
    return false;
  }
  // Re-installing the last accepted version is allowed only to restore a
  // missing or corrupt local copy.
  return version > accepted_version ||
         (version == accepted_version && !remote_active);
}

void KoFilterListUpdater::Start() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!base_url_.is_valid() || !factory_) {
    LOG(ERROR) << "K-O filter list channel disabled: invalid URL";
    return;
  }
  ScheduleCheck(kInitialDelay + base::RandTimeDeltaUpTo(base::Minutes(1)));
}

void KoFilterListUpdater::ScheduleCheck(base::TimeDelta delay) {
  timer_.Start(FROM_HERE, delay,
               base::BindOnce(&KoFilterListUpdater::CheckNow,
                              weak_factory_.GetWeakPtr()));
}

void KoFilterListUpdater::CheckNow() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (pending_ || !base_url_.is_valid()) {
    return;
  }
  if (!source_->IsReady()) {
    ScheduleCheck(kInitialDelay);
    return;
  }
  pending_ = std::make_unique<Pending>();
  Fetch(kManifestFile, kKoMaxManifestBytes,
        base::BindOnce(&KoFilterListUpdater::OnManifest,
                       weak_factory_.GetWeakPtr()));
}

void KoFilterListUpdater::Fetch(
    const std::string& file,
    size_t max_size,
    base::OnceCallback<void(std::optional<std::string>)> callback) {
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = base_url_.Resolve(file);
  request->method = "GET";
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  // Never follow redirects, so every byte comes from the configured origin.
  request->redirect_mode = network::mojom::RedirectMode::kError;
  request->load_flags = net::LOAD_BYPASS_CACHE | net::LOAD_DISABLE_CACHE;
  loader_ =
      network::SimpleURLLoader::Create(std::move(request), kTrafficAnnotation);
  loader_->SetTimeoutDuration(kRequestTimeout);
  loader_->DownloadToString(factory_.get(), std::move(callback), max_size);
}

void KoFilterListUpdater::OnManifest(std::optional<std::string> body) {
  if (!body) {
    return Finish(false, "manifest download failed");
  }
  pending_->manifest_json = std::move(*body);
  Fetch(kSignatureFile, kMaxSignatureBytes,
        base::BindOnce(&KoFilterListUpdater::OnSignature,
                       weak_factory_.GetWeakPtr()));
}

void KoFilterListUpdater::OnSignature(std::optional<std::string> body) {
  if (!body) {
    return Finish(false, "signature download failed");
  }
  pending_->signature = std::move(*body);
  if (!VerifyKoManifestSignature(pending_->manifest_json, pending_->signature,
                                 source_->public_key())) {
    return Finish(false, "manifest signature is invalid");
  }
  auto manifest = KoFilterListManifest::Parse(pending_->manifest_json);
  if (!manifest.has_value()) {
    return Finish(false, "manifest is malformed");
  }
  const bool remote_active = source_->is_remote_active();
  if (!ShouldAccept(manifest->version, source_->embedded_version(),
                    GetAcceptedVersion(local_state_), remote_active,
                    source_->manifest().version)) {
    return Finish(true, nullptr);  // Nothing newer.
  }
  if (!manifest->CoversCatalogLists(GetKoLockedFilterLists())) {
    return Finish(false, "manifest drops a core list");
  }
  for (const auto& entry : manifest->lists) {
    if (entry.kind == KoListKind::kCatalog) {
      return Finish(false, "catalog entries are not accepted");
    }
  }
  pending_->manifest = std::move(manifest).value();
  FetchNextList();
}

void KoFilterListUpdater::FetchNextList() {
  const auto& lists = pending_->manifest.lists;
  if (pending_->next >= lists.size()) {
    source_->file_task_runner()->PostTaskAndReplyWithResult(
        FROM_HERE,
        base::BindOnce(&InstallRemoteSet, source_->remote_dir(),
                       pending_->manifest_json, pending_->signature,
                       std::move(pending_->files)),
        base::BindOnce(&KoFilterListUpdater::OnInstalled,
                       weak_factory_.GetWeakPtr()));
    return;
  }
  const auto& entry = lists[pending_->next];
  Fetch(
      entry.file, entry.size,
      base::BindOnce(&KoFilterListUpdater::OnList, weak_factory_.GetWeakPtr()));
}

void KoFilterListUpdater::OnList(std::optional<std::string> body) {
  const auto& entry = pending_->manifest.lists[pending_->next];
  if (!body || !KoListMatchesDigest(*body, entry)) {
    return Finish(false, "list failed verification");
  }
  if (entry.kind == KoListKind::kFilters && KoListHasUnsafeException(*body)) {
    return Finish(false, "list allows every request");
  }
  pending_->files.emplace_back(entry.file, std::move(*body));
  ++pending_->next;
  FetchNextList();
}

void KoFilterListUpdater::OnInstalled(bool success) {
  if (!success) {
    return Finish(false, "could not store the snapshot");
  }
  local_state_->SetString(kKoFilterListsAcceptedVersion,
                          base::NumberToString(pending_->manifest.version));
  source_->OnRemoteSetInstalled(std::move(pending_->manifest));
  on_installed_.Run();
  Finish(true, nullptr);
}

void KoFilterListUpdater::Finish(bool success, const char* reason) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!success) {
    LOG(WARNING) << "K-O filter list update failed: " << reason;
  }
  loader_.reset();
  pending_.reset();
  ScheduleCheck(success ? kCheckInterval : kRetryInterval);
}

}  // namespace brave_shields
