// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef BRAVE_COMPONENTS_BRAVE_SHIELDS_CORE_BROWSER_KO_KO_FILTER_LIST_SOURCE_H_
#define BRAVE_COMPONENTS_BRAVE_SHIELDS_CORE_BROWSER_KO_KO_FILTER_LIST_SOURCE_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/sequence_checker.h"
#include "brave/components/brave_shields/core/browser/ko/ko_filter_list_manifest.h"

namespace base {
class SequencedTaskRunner;
}  // namespace base

namespace brave_shields {

// Catalog lists that K-O always enables and that cannot be unchecked.
bool IsKoLockedFilterList(std::string_view uuid);
std::vector<std::string_view> GetKoLockedFilterLists();

// Returns an embedded snapshot file by list id, or nullopt. Thread-safe.
std::optional<std::string> LoadKoEmbeddedList(std::string_view id);

// Verifies a channel snapshot stored in `dir` (manifest.json,
// manifest.json.sig and every listed file). Blocking.
std::optional<KoFilterListManifest> LoadVerifiedKoRemoteSet(
    const base::FilePath& dir,
    std::string_view public_key_hex,
    int64_t min_version);

// The K-O filter list data in effect: the embedded snapshot, or a newer
// verified snapshot from the update channel. Lives on the UI sequence.
class KoFilterListSource {
 public:
  class Observer : public base::CheckedObserver {
   public:
    virtual void OnKoFilterListsChanged() = 0;
  };

  // Loads an embedded file by list id. Called on a blocking sequence.
  using EmbeddedLoader =
      base::RepeatingCallback<std::optional<std::string>(const std::string&)>;

  // `remote_dir` holds verified channel data; it and `public_key_hex` may be
  // empty, which disables the channel.
  KoFilterListSource(std::string_view embedded_manifest_json,
                     EmbeddedLoader embedded_loader,
                     base::FilePath remote_dir,
                     std::string public_key_hex);
  KoFilterListSource(const KoFilterListSource&) = delete;
  KoFilterListSource& operator=(const KoFilterListSource&) = delete;
  ~KoFilterListSource();

  // Production instance backed by the resource bundle and the channel key
  // compiled in via the ko_filter_lists_pubkey GN arg.
  static std::unique_ptr<KoFilterListSource> Create(
      const base::FilePath& remote_dir);

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // False until any stored channel data has been checked.
  bool IsReady() const;
  // The set in effect. Empty if the embedded manifest is unusable.
  const KoFilterListManifest& manifest() const;
  int64_t embedded_version() const;
  bool is_remote_active() const;
  const base::FilePath& remote_dir() const { return remote_dir_; }
  const std::string& public_key() const { return public_key_hex_; }
  // Sequence for all reads and writes of remote_dir().
  base::SequencedTaskRunner* file_task_runner() const {
    return file_task_runner_.get();
  }

  // Reads `entry` from the active set and checks its digest. A corrupt
  // channel file falls back to the embedded copy and deactivates the channel.
  void LoadList(const KoFilterListEntry& entry,
                base::OnceCallback<void(std::optional<std::string>)> callback);
  // Scriptlet/redirect resources JSON of the active set.
  void LoadResources(
      base::OnceCallback<void(std::optional<std::string>)> callback);

  // Called after a verified channel snapshot was written to remote_dir().
  void OnRemoteSetInstalled(KoFilterListManifest manifest);

  base::WeakPtr<KoFilterListSource> AsWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

 private:
  void OnRemoteSetLoaded(std::optional<KoFilterListManifest> manifest);
  void OnListRead(KoFilterListEntry entry,
                  bool from_remote,
                  base::OnceCallback<void(std::optional<std::string>)> callback,
                  std::optional<std::string> data);
  void DeactivateRemote();
  void NotifyChanged();

  const EmbeddedLoader embedded_loader_;
  const base::FilePath remote_dir_;
  const std::string public_key_hex_;
  scoped_refptr<base::SequencedTaskRunner> file_task_runner_;

  KoFilterListManifest embedded_;
  std::optional<KoFilterListManifest> remote_;
  bool ready_ = false;

  base::ObserverList<Observer> observers_;
  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<KoFilterListSource> weak_factory_{this};
};

}  // namespace brave_shields

#endif  // BRAVE_COMPONENTS_BRAVE_SHIELDS_CORE_BROWSER_KO_KO_FILTER_LIST_SOURCE_H_
