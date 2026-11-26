// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/brave_shields/core/browser/ko/ko_filter_list_source.h"

#include <iterator>
#include <string_view>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "brave/components/brave_shields/core/common/brave_shield_constants.h"
#include "brave/components/ko/shields/buildflags/buildflags.h"
#include "brave/ko/shields/grit/ko_shields_resources.h"
#include "ui/base/resource/resource_bundle.h"

namespace brave_shields {

namespace {

constexpr char kManifestFile[] = "manifest.json";
constexpr char kSignatureFile[] = "manifest.json.sig";
constexpr size_t kMaxSignatureFileBytes = 1024;

// Privacy filters; uuid from Brave's list catalog.
constexpr char kDefaultPrivacyListUuid[] =
    "4D715457-307C-4383-8873-73A2FD263F71";

constexpr std::string_view kKoLockedFilterLists[] = {
    kDefaultAdblockFiltersListUuid,
    kDefaultPrivacyListUuid,
    kFirstPartyAdblockFiltersListUuid,
    kCookieListUuid,
};

struct KoEmbeddedList {
  std::string_view id;
  int resource_id;
};

#define KO_EMBEDDED_LIST(id, resource_id) {id, resource_id},
constexpr KoEmbeddedList kKoEmbeddedLists[] = {
#include "brave/ko/shields/ko_embedded_lists.inc"
};
#undef KO_EMBEDDED_LIST

std::optional<std::string> ReadVerifiedFile(base::FilePath path,
                                            KoFilterListEntry entry) {
  std::string data;
  if (!base::ReadFileToStringWithMaxSize(path, &data, entry.size) ||
      !KoListMatchesDigest(data, entry)) {
    LOG(ERROR) << "K-O filter list failed verification: " << entry.id;
    return std::nullopt;
  }
  return data;
}

std::optional<std::string> LoadVerifiedEmbedded(
    KoFilterListSource::EmbeddedLoader loader,
    KoFilterListEntry entry) {
  std::optional<std::string> data = loader.Run(entry.id);
  if (!data || !KoListMatchesDigest(*data, entry)) {
    LOG(ERROR) << "K-O embedded filter list failed verification: " << entry.id;
    return std::nullopt;
  }
  return data;
}

std::optional<KoFilterListManifest> LoadRemoteSetOnFileSequence(
    base::FilePath dir,
    std::string public_key_hex,
    int64_t min_version) {
  auto manifest = LoadVerifiedKoRemoteSet(dir, public_key_hex, min_version);
  if (!manifest && base::PathExists(dir)) {
    // Superseded or tampered with; the embedded snapshot takes over.
    base::DeletePathRecursively(dir);
  }
  return manifest;
}

}  // namespace

bool IsKoLockedFilterList(std::string_view uuid) {
  for (std::string_view locked : kKoLockedFilterLists) {
    if (uuid == locked) {
      return true;
    }
  }
  return false;
}

std::vector<std::string_view> GetKoLockedFilterLists() {
  return {std::begin(kKoLockedFilterLists), std::end(kKoLockedFilterLists)};
}

std::optional<std::string> LoadKoEmbeddedList(std::string_view id) {
  if (!ui::ResourceBundle::HasSharedInstance()) {
    return std::nullopt;
  }
  for (const auto& list : kKoEmbeddedLists) {
    if (list.id == id) {
      std::string data =
          ui::ResourceBundle::GetSharedInstance().LoadDataResourceString(
              list.resource_id);
      if (data.empty()) {
        return std::nullopt;
      }
      return data;
    }
  }
  return std::nullopt;
}

std::optional<KoFilterListManifest> LoadVerifiedKoRemoteSet(
    const base::FilePath& dir,
    std::string_view public_key_hex,
    int64_t min_version) {
  if (dir.empty() || !IsValidKoPublicKey(public_key_hex)) {
    return std::nullopt;
  }
  std::string manifest_json;
  std::string signature;
  if (!base::ReadFileToStringWithMaxSize(dir.AppendASCII(kManifestFile),
                                         &manifest_json, kKoMaxManifestBytes) ||
      !base::ReadFileToStringWithMaxSize(dir.AppendASCII(kSignatureFile),
                                         &signature, kMaxSignatureFileBytes)) {
    return std::nullopt;
  }
  if (!VerifyKoManifestSignature(manifest_json, signature, public_key_hex)) {
    LOG(ERROR) << "K-O filter list channel: stored signature is invalid";
    return std::nullopt;
  }
  auto manifest = KoFilterListManifest::Parse(manifest_json);
  if (!manifest.has_value() || manifest->version <= min_version ||
      !manifest->CoversCatalogLists(GetKoLockedFilterLists())) {
    return std::nullopt;
  }
  for (const auto& entry : manifest->lists) {
    if (entry.kind == KoListKind::kCatalog) {
      return std::nullopt;
    }
    std::optional<std::string> data =
        ReadVerifiedFile(dir.AppendASCII(entry.file), entry);
    if (!data || (entry.kind == KoListKind::kFilters &&
                  KoListHasUnsafeException(*data))) {
      return std::nullopt;
    }
  }
  return std::move(manifest).value();
}

KoFilterListSource::KoFilterListSource(std::string_view embedded_manifest_json,
                                       EmbeddedLoader embedded_loader,
                                       base::FilePath remote_dir,
                                       std::string public_key_hex)
    : embedded_loader_(std::move(embedded_loader)),
      remote_dir_(std::move(remote_dir)),
      public_key_hex_(std::move(public_key_hex)),
      file_task_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN})) {
  auto parsed = KoFilterListManifest::Parse(embedded_manifest_json);
  if (parsed.has_value()) {
    embedded_ = std::move(parsed).value();
  } else {
    LOG(ERROR) << "K-O embedded filter list manifest: " << parsed.error();
  }

  if (remote_dir_.empty() || !IsValidKoPublicKey(public_key_hex_)) {
    ready_ = true;
    return;
  }
  file_task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&LoadRemoteSetOnFileSequence, remote_dir_, public_key_hex_,
                     embedded_.version),
      base::BindOnce(&KoFilterListSource::OnRemoteSetLoaded,
                     weak_factory_.GetWeakPtr()));
}

KoFilterListSource::~KoFilterListSource() = default;

// static
std::unique_ptr<KoFilterListSource> KoFilterListSource::Create(
    const base::FilePath& remote_dir) {
  std::string manifest_json;
  if (ui::ResourceBundle::HasSharedInstance()) {
    manifest_json =
        ui::ResourceBundle::GetSharedInstance().LoadDataResourceString(
            IDR_KO_SHIELDS_MANIFEST);
  }
  return std::make_unique<KoFilterListSource>(
      manifest_json, base::BindRepeating([](const std::string& id) {
        return LoadKoEmbeddedList(id);
      }),
      remote_dir, BUILDFLAG(KO_FILTER_LISTS_PUBKEY));
}

void KoFilterListSource::AddObserver(Observer* observer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  observers_.AddObserver(observer);
}

void KoFilterListSource::RemoveObserver(Observer* observer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  observers_.RemoveObserver(observer);
}

bool KoFilterListSource::IsReady() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return ready_;
}

const KoFilterListManifest& KoFilterListSource::manifest() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return remote_ ? *remote_ : embedded_;
}

int64_t KoFilterListSource::embedded_version() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return embedded_.version;
}

bool KoFilterListSource::is_remote_active() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return remote_.has_value();
}

void KoFilterListSource::LoadList(
    const KoFilterListEntry& entry,
    base::OnceCallback<void(std::optional<std::string>)> callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const bool from_remote = remote_.has_value() && remote_->Find(entry.id);
  auto reply = base::BindOnce(&KoFilterListSource::OnListRead,
                              weak_factory_.GetWeakPtr(), entry, from_remote,
                              std::move(callback));
  if (from_remote) {
    file_task_runner_->PostTaskAndReplyWithResult(
        FROM_HERE,
        base::BindOnce(&ReadVerifiedFile, remote_dir_.AppendASCII(entry.file),
                       entry),
        std::move(reply));
    return;
  }
  file_task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&LoadVerifiedEmbedded, embedded_loader_, entry),
      std::move(reply));
}

void KoFilterListSource::LoadResources(
    base::OnceCallback<void(std::optional<std::string>)> callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  for (const auto& entry : manifest().lists) {
    if (entry.kind == KoListKind::kResources) {
      LoadList(entry, std::move(callback));
      return;
    }
  }
  std::move(callback).Run(std::nullopt);
}

void KoFilterListSource::OnRemoteSetInstalled(KoFilterListManifest manifest) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  remote_ = std::move(manifest);
  NotifyChanged();
}

void KoFilterListSource::OnRemoteSetLoaded(
    std::optional<KoFilterListManifest> manifest) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  remote_ = std::move(manifest);
  ready_ = true;
  NotifyChanged();
}

void KoFilterListSource::OnListRead(
    KoFilterListEntry entry,
    bool from_remote,
    base::OnceCallback<void(std::optional<std::string>)> callback,
    std::optional<std::string> data) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!data && from_remote && remote_) {
    // A stored channel file changed after it was verified. Fall back to the
    // embedded snapshot; observers reload from it.
    DeactivateRemote();
  }
  std::move(callback).Run(std::move(data));
}

void KoFilterListSource::DeactivateRemote() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  remote_.reset();
  file_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(base::IgnoreResult(&base::DeletePathRecursively),
                     remote_dir_));
  NotifyChanged();
}

void KoFilterListSource::NotifyChanged() {
  observers_.Notify(&Observer::OnKoFilterListsChanged);
}

}  // namespace brave_shields
