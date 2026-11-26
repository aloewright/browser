// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/ko/machines/browser/credential_store.h"

#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/values.h"
#include "brave/components/ko/machines/core/pref_names.h"
#include "components/os_crypt/async/common/encryptor.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"

namespace ko_machines {

namespace {

constexpr char kUsernameKey[] = "u";
constexpr char kPasswordKey[] = "p";

}  // namespace

bool AreValidCredentials(const MachineCredentials& credentials) {
  return !credentials.password.empty() &&
         credentials.password.size() <= kMaxPasswordLength &&
         credentials.username.size() <= kMaxUsernameLength;
}

std::optional<std::string> EncryptCredentials(
    const os_crypt_async::Encryptor& encryptor,
    const MachineCredentials& credentials) {
  if (!AreValidCredentials(credentials) || !encryptor.IsEncryptionAvailable()) {
    return std::nullopt;
  }
  std::optional<std::string> plaintext =
      base::WriteJson(base::DictValue()
                          .Set(kUsernameKey, credentials.username)
                          .Set(kPasswordKey, credentials.password));
  if (!plaintext) {
    return std::nullopt;
  }
  std::optional<std::vector<uint8_t>> ciphertext =
      encryptor.EncryptString(*plaintext);
  if (!ciphertext) {
    return std::nullopt;
  }
  return base::Base64Encode(*ciphertext);
}

std::optional<MachineCredentials> DecryptCredentials(
    const os_crypt_async::Encryptor& encryptor,
    std::string_view stored) {
  std::optional<std::vector<uint8_t>> ciphertext = base::Base64Decode(stored);
  if (!ciphertext) {
    return std::nullopt;
  }
  std::optional<std::string> plaintext = encryptor.DecryptData(*ciphertext);
  if (!plaintext) {
    return std::nullopt;
  }
  std::optional<base::DictValue> dict =
      base::JSONReader::ReadDict(*plaintext, base::JSON_PARSE_RFC);
  if (!dict) {
    return std::nullopt;
  }
  const std::string* username = dict->FindString(kUsernameKey);
  const std::string* password = dict->FindString(kPasswordKey);
  if (!username || !password) {
    return std::nullopt;
  }
  MachineCredentials credentials{*username, *password};
  if (!AreValidCredentials(credentials)) {
    return std::nullopt;
  }
  return credentials;
}

bool HasStoredCredentials(const PrefService& prefs, std::string_view id) {
  return GetStoredCredentials(prefs, id).has_value();
}

std::optional<std::string> GetStoredCredentials(const PrefService& prefs,
                                                std::string_view id) {
  const std::string* stored = prefs.GetDict(prefs::kCredentials).FindString(id);
  if (!stored || stored->empty()) {
    return std::nullopt;
  }
  return *stored;
}

void StoreCredentials(PrefService& prefs,
                      std::string_view id,
                      std::string ciphertext) {
  ScopedDictPrefUpdate update(&prefs, prefs::kCredentials);
  update->Set(id, std::move(ciphertext));
}

void ClearStoredCredentials(PrefService& prefs, std::string_view id) {
  ScopedDictPrefUpdate update(&prefs, prefs::kCredentials);
  update->Remove(id);
}

}  // namespace ko_machines
