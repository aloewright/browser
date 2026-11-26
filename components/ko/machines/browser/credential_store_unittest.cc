// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/ko/machines/browser/credential_store.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/base64.h"
#include "brave/components/ko/machines/core/pref_names.h"
#include "brave/components/ko/machines/core/prefs.h"
#include "components/os_crypt/async/browser/test_utils.h"
#include "components/prefs/testing_pref_service.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ko_machines {

namespace {

constexpr char kPassword[] = "correct horse battery";
constexpr char kUsername[] = "alex";

bool Contains(const std::vector<uint8_t>& haystack, std::string_view needle) {
  return std::string(haystack.begin(), haystack.end()).find(needle) !=
         std::string::npos;
}

}  // namespace

TEST(KOCredentialStoreTest, RoundTrip) {
  auto encryptor = os_crypt_async::GetTestEncryptorForTesting();
  std::optional<std::string> stored =
      EncryptCredentials(*encryptor, {kUsername, kPassword});
  ASSERT_TRUE(stored);

  // Neither the stored string nor the decoded bytes hold the plaintext.
  EXPECT_EQ(std::string::npos, stored->find(kPassword));
  std::optional<std::vector<uint8_t>> bytes = base::Base64Decode(*stored);
  ASSERT_TRUE(bytes);
  EXPECT_FALSE(Contains(*bytes, kPassword));
  EXPECT_FALSE(Contains(*bytes, kUsername));

  std::optional<MachineCredentials> decrypted =
      DecryptCredentials(*encryptor, *stored);
  ASSERT_TRUE(decrypted);
  EXPECT_EQ(kUsername, decrypted->username);
  EXPECT_EQ(kPassword, decrypted->password);
}

TEST(KOCredentialStoreTest, PasswordOnly) {
  auto encryptor = os_crypt_async::GetTestEncryptorForTesting();
  std::optional<std::string> stored =
      EncryptCredentials(*encryptor, {"", "vncpass"});
  ASSERT_TRUE(stored);
  std::optional<MachineCredentials> decrypted =
      DecryptCredentials(*encryptor, *stored);
  ASSERT_TRUE(decrypted);
  EXPECT_TRUE(decrypted->username.empty());
  EXPECT_EQ("vncpass", decrypted->password);
}

TEST(KOCredentialStoreTest, FailsClosedWithoutEncryption) {
  auto encryptor = os_crypt_async::GetTestEncryptorWithoutKeysForTesting();
  EXPECT_FALSE(EncryptCredentials(*encryptor, {kUsername, kPassword}));
}

TEST(KOCredentialStoreTest, RejectsInvalidCredentials) {
  auto encryptor = os_crypt_async::GetTestEncryptorForTesting();
  EXPECT_FALSE(EncryptCredentials(*encryptor, {kUsername, ""}));
  EXPECT_FALSE(EncryptCredentials(
      *encryptor, {kUsername, std::string(kMaxPasswordLength + 1, 'p')}));
  EXPECT_FALSE(EncryptCredentials(
      *encryptor, {std::string(kMaxUsernameLength + 1, 'u'), kPassword}));
}

TEST(KOCredentialStoreTest, DecryptRejectsGarbageAndOtherKeys) {
  auto encryptor = os_crypt_async::GetTestEncryptorForTesting();
  EXPECT_FALSE(DecryptCredentials(*encryptor, ""));
  EXPECT_FALSE(DecryptCredentials(*encryptor, "not base64!"));
  EXPECT_FALSE(DecryptCredentials(*encryptor, base::Base64Encode("plain")));

  std::optional<std::string> stored =
      EncryptCredentials(*encryptor, {kUsername, kPassword});
  ASSERT_TRUE(stored);
  auto other = os_crypt_async::GetTestEncryptorForTesting();
  EXPECT_FALSE(DecryptCredentials(*other, *stored));
}

TEST(KOCredentialStoreTest, PrefStorage) {
  TestingPrefServiceSimple pref_service;
  RegisterProfilePrefs(pref_service.registry());

  EXPECT_FALSE(HasStoredCredentials(pref_service, "a"));
  StoreCredentials(pref_service, "a", "ciphertext");
  EXPECT_TRUE(HasStoredCredentials(pref_service, "a"));
  EXPECT_EQ("ciphertext", GetStoredCredentials(pref_service, "a"));
  EXPECT_FALSE(HasStoredCredentials(pref_service, "b"));

  ClearStoredCredentials(pref_service, "a");
  EXPECT_FALSE(HasStoredCredentials(pref_service, "a"));
  EXPECT_TRUE(pref_service.GetDict(prefs::kCredentials).empty());
}

}  // namespace ko_machines
