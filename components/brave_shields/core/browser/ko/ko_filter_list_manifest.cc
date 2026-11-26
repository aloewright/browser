// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/brave_shields/core/browser/ko/ko_filter_list_manifest.h"

#include <array>
#include <cmath>
#include <optional>
#include <utility>

#include "base/containers/flat_set.h"
#include "base/containers/span.h"
#include "base/json/json_reader.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "crypto/hash.h"
#include "third_party/boringssl/src/include/openssl/curve25519.h"

namespace brave_shields {

namespace {

constexpr size_t kMaxIdLength = 64;
constexpr size_t kMaxTitleLength = 256;
// Largest integer a double holds exactly.
constexpr double kMaxSafeInteger = 9007199254740991.0;

bool IsValidId(std::string_view id) {
  if (id.empty() || id.size() > kMaxIdLength) {
    return false;
  }
  for (size_t i = 0; i < id.size(); ++i) {
    const char c = id[i];
    const bool ok =
        base::IsAsciiLower(c) || base::IsAsciiDigit(c) || (c == '-' && i > 0);
    if (!ok) {
      return false;
    }
  }
  return true;
}

bool IsValidCatalogUuid(std::string_view uuid) {
  if (uuid.empty() || uuid.size() > kMaxIdLength) {
    return false;
  }
  for (const char c : uuid) {
    if (!base::IsAsciiAlphaNumeric(c) && c != '-') {
      return false;
    }
  }
  return true;
}

bool IsLowerHex(std::string_view s, size_t length) {
  if (s.size() != length) {
    return false;
  }
  for (const char c : s) {
    if (!base::IsAsciiDigit(c) && !(c >= 'a' && c <= 'f')) {
      return false;
    }
  }
  return true;
}

std::optional<KoListKind> ParseKind(const std::string& kind) {
  if (kind == "filters") {
    return KoListKind::kFilters;
  }
  if (kind == "resources") {
    return KoListKind::kResources;
  }
  if (kind == "catalog") {
    return KoListKind::kCatalog;
  }
  return std::nullopt;
}

std::optional<KoListEngine> ParseEngine(const std::string& engine) {
  if (engine == "default") {
    return KoListEngine::kDefault;
  }
  if (engine == "additional") {
    return KoListEngine::kAdditional;
  }
  if (engine == "both") {
    return KoListEngine::kBoth;
  }
  return std::nullopt;
}

base::expected<KoFilterListEntry, std::string> ParseEntry(
    const base::DictValue& dict) {
  KoFilterListEntry entry;

  const std::string* id = dict.FindString("id");
  if (!id || !IsValidId(*id)) {
    return base::unexpected("invalid id");
  }
  entry.id = *id;

  if (const std::string* title = dict.FindString("title")) {
    if (title->size() > kMaxTitleLength || !base::IsStringUTF8(*title)) {
      return base::unexpected("invalid title for " + entry.id);
    }
    entry.title = *title;
  }

  const std::string* kind = dict.FindString("kind");
  const auto parsed_kind = kind ? ParseKind(*kind) : std::nullopt;
  if (!parsed_kind) {
    return base::unexpected("invalid kind for " + entry.id);
  }
  entry.kind = *parsed_kind;

  // The file name is derived from the id so it can never escape the list
  // directory.
  const std::string expected_file =
      entry.id + (entry.kind == KoListKind::kFilters ? ".txt" : ".json");
  const std::string* file = dict.FindString("file");
  if (!file || *file != expected_file) {
    return base::unexpected("invalid file for " + entry.id);
  }
  entry.file = *file;

  const std::string* sha256 = dict.FindString("sha256");
  if (!sha256 || !IsLowerHex(*sha256, 2 * crypto::hash::kSha256Size)) {
    return base::unexpected("invalid sha256 for " + entry.id);
  }
  entry.sha256 = *sha256;

  const std::optional<int> size = dict.FindInt("size");
  if (!size || *size <= 0 || static_cast<size_t>(*size) > kKoMaxListBytes) {
    return base::unexpected("invalid size for " + entry.id);
  }
  entry.size = static_cast<size_t>(*size);

  if (entry.kind != KoListKind::kFilters) {
    return entry;
  }

  const std::string* engine = dict.FindString("engine");
  const auto parsed_engine = engine ? ParseEngine(*engine) : std::nullopt;
  if (!parsed_engine) {
    return base::unexpected("invalid engine for " + entry.id);
  }
  entry.engine = *parsed_engine;

  const std::optional<int> mask = dict.FindInt("permission_mask");
  if (!mask || *mask < 0 || *mask > 0xff) {
    return base::unexpected("invalid permission_mask for " + entry.id);
  }
  entry.permission_mask = static_cast<uint8_t>(*mask);

  if (const std::string* replaces = dict.FindString("replaces");
      replaces && !replaces->empty()) {
    if (!IsValidCatalogUuid(*replaces)) {
      return base::unexpected("invalid replaces for " + entry.id);
    }
    entry.replaces = *replaces;
  }
  return entry;
}

// Network pattern that matches every URL, e.g. "*", "||*^", "|https://".
bool IsUniversalPattern(std::string_view pattern) {
  pattern = base::TrimWhitespaceASCII(pattern, base::TRIM_ALL);
  if (pattern == "/.*/" || pattern == "/^/") {
    return true;
  }
  const std::string_view core =
      base::TrimString(pattern, "|*^/:", base::TRIM_ALL);
  return core.empty() || core == "http" || core == "https" || core == "ws" ||
         core == "wss";
}

// Options restrict the rule to specific sites (non-negated domain=/from=/to=).
bool IsScoped(std::string_view options) {
  for (std::string_view option : base::SplitStringPiece(
           options, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    const size_t eq = option.find('=');
    if (eq == std::string_view::npos) {
      continue;
    }
    const std::string_view name = option.substr(0, eq);
    if (name != "domain" && name != "from" && name != "to") {
      continue;
    }
    for (std::string_view domain : base::SplitStringPiece(
             option.substr(eq + 1), "|", base::TRIM_WHITESPACE,
             base::SPLIT_WANT_NONEMPTY)) {
      if (domain[0] != '~') {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

KoFilterListEntry::KoFilterListEntry() = default;
KoFilterListEntry::~KoFilterListEntry() = default;
KoFilterListEntry::KoFilterListEntry(const KoFilterListEntry&) = default;
KoFilterListEntry& KoFilterListEntry::operator=(const KoFilterListEntry&) =
    default;
KoFilterListEntry::KoFilterListEntry(KoFilterListEntry&&) = default;
KoFilterListEntry& KoFilterListEntry::operator=(KoFilterListEntry&&) = default;

bool KoFilterListEntry::AppliesToEngine(bool is_default_engine) const {
  if (kind != KoListKind::kFilters) {
    return false;
  }
  switch (engine) {
    case KoListEngine::kBoth:
      return true;
    case KoListEngine::kDefault:
      return is_default_engine;
    case KoListEngine::kAdditional:
      return !is_default_engine;
  }
  return false;
}

KoFilterListManifest::KoFilterListManifest() = default;
KoFilterListManifest::~KoFilterListManifest() = default;
KoFilterListManifest::KoFilterListManifest(const KoFilterListManifest&) =
    default;
KoFilterListManifest& KoFilterListManifest::operator=(
    const KoFilterListManifest&) = default;
KoFilterListManifest::KoFilterListManifest(KoFilterListManifest&&) = default;
KoFilterListManifest& KoFilterListManifest::operator=(KoFilterListManifest&&) =
    default;

// static
base::expected<KoFilterListManifest, std::string> KoFilterListManifest::Parse(
    std::string_view json) {
  if (json.size() > kKoMaxManifestBytes) {
    return base::unexpected("manifest too large");
  }
  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!root) {
    return base::unexpected("manifest is not a JSON object");
  }
  if (root->FindInt("schema") != 1) {
    return base::unexpected("unsupported schema");
  }
  const std::string* channel = root->FindString("channel");
  if (!channel || *channel != kKoFilterListChannel) {
    return base::unexpected("wrong channel");
  }
  // Versions exceed int32, so JSON numbers arrive as doubles.
  const std::optional<double> version = root->FindDouble("version");
  if (!version || *version < 1 || *version > kMaxSafeInteger ||
      std::floor(*version) != *version) {
    return base::unexpected("invalid version");
  }

  const base::ListValue* lists = root->FindList("lists");
  if (!lists || lists->empty() || lists->size() > kKoMaxLists) {
    return base::unexpected("invalid lists");
  }

  KoFilterListManifest manifest;
  manifest.version = static_cast<int64_t>(*version);
  base::flat_set<std::string> ids;
  size_t total = 0;
  for (const base::Value& value : *lists) {
    const base::DictValue* dict = value.GetIfDict();
    if (!dict) {
      return base::unexpected("list entry is not an object");
    }
    auto entry = ParseEntry(*dict);
    if (!entry.has_value()) {
      return base::unexpected(std::move(entry).error());
    }
    if (!ids.insert(entry->id).second) {
      return base::unexpected("duplicate id " + entry->id);
    }
    total += entry->size;
    if (total > kKoMaxTotalBytes) {
      return base::unexpected("lists too large");
    }
    manifest.lists.push_back(std::move(entry).value());
  }
  return manifest;
}

const KoFilterListEntry* KoFilterListManifest::Find(std::string_view id) const {
  for (const auto& entry : lists) {
    if (entry.id == id) {
      return &entry;
    }
  }
  return nullptr;
}

bool KoFilterListManifest::CoversCatalogLists(
    const std::vector<std::string_view>& uuids) const {
  for (std::string_view uuid : uuids) {
    bool covered = false;
    for (const auto& entry : lists) {
      if (entry.kind == KoListKind::kFilters && entry.replaces == uuid) {
        covered = true;
        break;
      }
    }
    if (!covered) {
      return false;
    }
  }
  return true;
}

bool IsValidKoPublicKey(std::string_view public_key_hex) {
  std::array<uint8_t, ED25519_PUBLIC_KEY_LEN> key{};
  return public_key_hex.size() == 2 * key.size() &&
         base::HexStringToSpan(public_key_hex, key);
}

bool VerifyKoManifestSignature(std::string_view manifest,
                               std::string_view signature_hex,
                               std::string_view public_key_hex) {
  std::array<uint8_t, ED25519_PUBLIC_KEY_LEN> public_key{};
  if (public_key_hex.size() != 2 * public_key.size() ||
      !base::HexStringToSpan(public_key_hex, public_key)) {
    return false;
  }
  signature_hex = base::TrimWhitespaceASCII(signature_hex, base::TRIM_ALL);
  std::array<uint8_t, ED25519_SIGNATURE_LEN> signature{};
  if (signature_hex.size() != 2 * signature.size() ||
      !base::HexStringToSpan(signature_hex, signature)) {
    return false;
  }
  std::string message(kKoManifestSignaturePrefix);
  message.append(manifest);
  const auto message_bytes = base::as_byte_span(message);
  return ED25519_verify(message_bytes.data(), message_bytes.size(),
                        signature.data(), public_key.data()) == 1;
}

bool KoListMatchesDigest(std::string_view data,
                         const KoFilterListEntry& entry) {
  return data.size() == entry.size &&
         base::HexEncodeLower(crypto::hash::Sha256(data)) == entry.sha256;
}

bool KoListHasUnsafeException(std::string_view list_text) {
  for (std::string_view line : base::SplitStringPiece(
           list_text, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    if (!line.starts_with("@@")) {
      continue;
    }
    line.remove_prefix(2);
    const size_t dollar = line.find('$');
    const std::string_view pattern = line.substr(0, dollar);
    const std::string_view options = dollar == std::string_view::npos
                                         ? std::string_view()
                                         : line.substr(dollar + 1);
    if (IsUniversalPattern(pattern) && !IsScoped(options)) {
      return true;
    }
  }
  return false;
}

}  // namespace brave_shields
