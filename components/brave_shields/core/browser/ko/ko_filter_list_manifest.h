// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef BRAVE_COMPONENTS_BRAVE_SHIELDS_CORE_BROWSER_KO_KO_FILTER_LIST_MANIFEST_H_
#define BRAVE_COMPONENTS_BRAVE_SHIELDS_CORE_BROWSER_KO_KO_FILTER_LIST_MANIFEST_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "base/types/expected.h"

namespace brave_shields {

// Manifest format shared by the embedded snapshot and the signed update
// channel; written by brave/ko/shields/update_lists.py.
inline constexpr char kKoFilterListChannel[] = "ko-filter-lists";
// Signatures cover this prefix followed by the exact manifest bytes.
inline constexpr char kKoManifestSignaturePrefix[] =
    "ko-filter-lists-manifest-v1\n";
inline constexpr size_t kKoMaxManifestBytes = 512 * 1024;
inline constexpr size_t kKoMaxListBytes = 32 * 1024 * 1024;
inline constexpr size_t kKoMaxTotalBytes = 128 * 1024 * 1024;
inline constexpr size_t kKoMaxLists = 256;

enum class KoListKind { kFilters, kResources, kCatalog };
enum class KoListEngine { kDefault, kAdditional, kBoth };

struct KoFilterListEntry {
  KoFilterListEntry();
  ~KoFilterListEntry();
  KoFilterListEntry(const KoFilterListEntry&);
  KoFilterListEntry& operator=(const KoFilterListEntry&);
  KoFilterListEntry(KoFilterListEntry&&);
  KoFilterListEntry& operator=(KoFilterListEntry&&);

  bool AppliesToEngine(bool is_default_engine) const;

  std::string id;
  std::string title;
  // Always `id` plus ".txt" or ".json"; safe to join to a directory.
  std::string file;
  // Lowercase hex SHA-256 of the file.
  std::string sha256;
  size_t size = 0;
  KoListKind kind = KoListKind::kFilters;
  KoListEngine engine = KoListEngine::kDefault;
  uint8_t permission_mask = 0;
  // Brave catalog uuid this list substitutes for, or empty for K-O extras.
  std::string replaces;
};

struct KoFilterListManifest {
  KoFilterListManifest();
  ~KoFilterListManifest();
  KoFilterListManifest(const KoFilterListManifest&);
  KoFilterListManifest& operator=(const KoFilterListManifest&);
  KoFilterListManifest(KoFilterListManifest&&);
  KoFilterListManifest& operator=(KoFilterListManifest&&);

  // Strict parser; rejects anything not produced by update_lists.py.
  static base::expected<KoFilterListManifest, std::string> Parse(
      std::string_view json);

  const KoFilterListEntry* Find(std::string_view id) const;
  // True if some list substitutes for every uuid in `uuids`.
  bool CoversCatalogLists(const std::vector<std::string_view>& uuids) const;

  // Monotonic, e.g. 202611261200 (UTC YYYYMMDDHHMM).
  int64_t version = 0;
  std::vector<KoFilterListEntry> lists;
};

// Verifies an ed25519 signature (128 hex chars, surrounding whitespace
// ignored) over kKoManifestSignaturePrefix + `manifest` with a 64 hex char
// public key. Fails closed on any malformed input.
bool VerifyKoManifestSignature(std::string_view manifest,
                               std::string_view signature_hex,
                               std::string_view public_key_hex);

bool IsValidKoPublicKey(std::string_view public_key_hex);

// Size and SHA-256 of `data` match `entry`.
bool KoListMatchesDigest(std::string_view data, const KoFilterListEntry& entry);

// True if the list has an unscoped exception that allows every request, e.g.
// "@@*" or "@@||*^$document". Mirrors unsafe_exceptions() in update_lists.py.
bool KoListHasUnsafeException(std::string_view list_text);

}  // namespace brave_shields

#endif  // BRAVE_COMPONENTS_BRAVE_SHIELDS_CORE_BROWSER_KO_KO_FILTER_LIST_MANIFEST_H_
