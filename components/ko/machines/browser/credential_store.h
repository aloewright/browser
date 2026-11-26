// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef BRAVE_COMPONENTS_KO_MACHINES_BROWSER_CREDENTIAL_STORE_H_
#define BRAVE_COMPONENTS_KO_MACHINES_BROWSER_CREDENTIAL_STORE_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

class PrefService;

namespace os_crypt_async {
class Encryptor;
}  // namespace os_crypt_async

namespace ko_machines {

inline constexpr size_t kMaxUsernameLength = 256;
inline constexpr size_t kMaxPasswordLength = 256;

struct MachineCredentials {
  std::string username;
  std::string password;
};

// True if the password is non-empty and both fit the limits.
bool AreValidCredentials(const MachineCredentials& credentials);

// Encrypts `credentials` with OSCrypt and returns base64 for prefs. nullopt if
// they are invalid or encryption is unavailable.
std::optional<std::string> EncryptCredentials(
    const os_crypt_async::Encryptor& encryptor,
    const MachineCredentials& credentials);
std::optional<MachineCredentials> DecryptCredentials(
    const os_crypt_async::Encryptor& encryptor,
    std::string_view stored);

// Per-machine entries in the kCredentials pref. Only ciphertext is stored.
bool HasStoredCredentials(const PrefService& prefs, std::string_view id);
std::optional<std::string> GetStoredCredentials(const PrefService& prefs,
                                                std::string_view id);
void StoreCredentials(PrefService& prefs,
                      std::string_view id,
                      std::string ciphertext);
void ClearStoredCredentials(PrefService& prefs, std::string_view id);

}  // namespace ko_machines

#endif  // BRAVE_COMPONENTS_KO_MACHINES_BROWSER_CREDENTIAL_STORE_H_
