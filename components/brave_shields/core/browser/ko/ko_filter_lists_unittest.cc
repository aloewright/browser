// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include <array>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "base/values.h"
#include "brave/components/brave_shields/core/browser/ad_block_filters_provider_manager.h"
#include "brave/components/brave_shields/core/browser/ko/ko_filter_list_manifest.h"
#include "brave/components/brave_shields/core/browser/ko/ko_filter_list_source.h"
#include "brave/components/brave_shields/core/browser/ko/ko_filter_list_updater.h"
#include "brave/components/brave_shields/core/browser/ko/ko_filters_provider.h"
#include "brave/components/brave_shields/core/common/adblock/rs/src/lib.rs.h"
#include "brave/components/brave_shields/core/common/brave_shield_constants.h"
#include "components/prefs/testing_pref_service.h"
#include "crypto/hash.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/test/test_url_loader_factory.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/boringssl/src/include/openssl/curve25519.h"
#include "url/gurl.h"

namespace brave_shields {

namespace {

// RFC 8032, section 7.1, TEST 1.
constexpr char kSeedHex[] =
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
constexpr char kPublicKeyHex[] =
    "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
// publish_lists.py signature of "{\"schema\":1}\n" with kSeedHex.
constexpr char kPythonSignatureHex[] =
    "119dd8d57183831f2a1eb32830f7c39fc8ce20eea0d3111d36338605a104adc1"
    "234d88c5b63598e432f7bb49bc083bd8f2aace6690cf78748fc73f64d94ef80e";

constexpr char kPrivacyUuid[] = "4D715457-307C-4383-8873-73A2FD263F71";
constexpr char kChannelUrl[] = "https://lists.example.com/k-o/";

struct TestList {
  std::string id;
  std::string content;
  std::string replaces;
  std::string engine = "default";
  std::string kind = "filters";
};

std::string Sha256Hex(std::string_view data) {
  return base::HexEncodeLower(crypto::hash::Sha256(data));
}

// Lists covering every locked catalog entry plus one K-O extra.
std::vector<TestList> CoreLists(std::string_view tag) {
  const std::string t(tag);
  return {
      {"ko-default", "||ads-" + t + ".example^\n",
       kDefaultAdblockFiltersListUuid},
      {"ko-privacy", "||track-" + t + ".example^\n", kPrivacyUuid},
      {"ko-firstparty", "||first-" + t + ".example^\n",
       kFirstPartyAdblockFiltersListUuid, "additional"},
      {"ko-cookie", "##.cookie-" + t + "\n", kCookieListUuid, "additional"},
      {"ko-extra", "||extra-" + t + ".example^\n", "", "both"},
  };
}

std::string BuildManifest(const std::vector<TestList>& lists, int64_t version) {
  base::ListValue entries;
  for (const auto& list : lists) {
    base::DictValue entry;
    entry.Set("id", list.id);
    entry.Set("title", list.id);
    entry.Set("kind", list.kind);
    entry.Set("file", list.id + (list.kind == "filters" ? ".txt" : ".json"));
    entry.Set("sha256", Sha256Hex(list.content));
    entry.Set("size", static_cast<int>(list.content.size()));
    entry.Set("source_url", "https://example.com/" + list.id);
    if (list.kind == "filters") {
      entry.Set("engine", list.engine);
      entry.Set("permission_mask", 1);
      entry.Set("replaces", list.replaces);
    }
    entries.Append(std::move(entry));
  }
  base::DictValue root;
  root.Set("schema", 1);
  root.Set("channel", kKoFilterListChannel);
  root.Set("version", static_cast<double>(version));
  root.Set("generated", "2026-11-26T00:00:00Z");
  root.Set("lists", std::move(entries));
  return base::WriteJson(root).value();
}

std::string Sign(std::string_view manifest) {
  std::array<uint8_t, 32> seed;
  EXPECT_TRUE(base::HexStringToSpan(kSeedHex, seed));
  std::array<uint8_t, ED25519_PUBLIC_KEY_LEN> public_key;
  std::array<uint8_t, ED25519_PRIVATE_KEY_LEN> private_key;
  ED25519_keypair_from_seed(public_key.data(), private_key.data(), seed.data());
  std::string message(kKoManifestSignaturePrefix);
  message.append(manifest);
  const auto bytes = base::as_byte_span(message);
  std::array<uint8_t, ED25519_SIGNATURE_LEN> signature;
  EXPECT_EQ(1, ED25519_sign(signature.data(), bytes.data(), bytes.size(),
                            private_key.data()));
  return base::HexEncodeLower(signature);
}

void WriteRemoteSet(const base::FilePath& dir,
                    const std::vector<TestList>& lists,
                    const std::string& manifest,
                    const std::string& signature) {
  ASSERT_TRUE(base::CreateDirectory(dir));
  for (const auto& list : lists) {
    ASSERT_TRUE(
        base::WriteFile(dir.AppendASCII(list.id + ".txt"), list.content));
  }
  ASSERT_TRUE(base::WriteFile(dir.AppendASCII("manifest.json"), manifest));
  ASSERT_TRUE(base::WriteFile(dir.AppendASCII("manifest.json.sig"), signature));
}

KoFilterListSource::EmbeddedLoader LoaderFor(
    const std::vector<TestList>& lists) {
  std::map<std::string, std::string> files;
  for (const auto& list : lists) {
    files[list.id] = list.content;
  }
  return base::BindRepeating(
      [](const std::map<std::string, std::string>& files,
         const std::string& id) -> std::optional<std::string> {
        auto it = files.find(id);
        if (it == files.end()) {
          return std::nullopt;
        }
        return it->second;
      },
      std::move(files));
}

std::vector<std::string> Ids(const std::vector<KoFilterListEntry>& entries) {
  std::vector<std::string> ids;
  for (const auto& entry : entries) {
    ids.push_back(entry.id);
  }
  return ids;
}

}  // namespace

// Manifest parsing and verification.

TEST(KoFilterListManifestTest, ParsesValidManifest) {
  const auto lists = CoreLists("a");
  auto manifest =
      KoFilterListManifest::Parse(BuildManifest(lists, 202611260000));
  ASSERT_TRUE(manifest.has_value()) << manifest.error();
  EXPECT_EQ(202611260000, manifest->version);
  ASSERT_EQ(lists.size(), manifest->lists.size());
  const KoFilterListEntry* cookie = manifest->Find("ko-cookie");
  ASSERT_TRUE(cookie);
  EXPECT_EQ("ko-cookie.txt", cookie->file);
  EXPECT_EQ(kCookieListUuid, cookie->replaces);
  EXPECT_FALSE(cookie->AppliesToEngine(true));
  EXPECT_TRUE(cookie->AppliesToEngine(false));
  EXPECT_TRUE(manifest->Find("ko-extra")->AppliesToEngine(true));
  EXPECT_TRUE(manifest->Find("ko-extra")->AppliesToEngine(false));
  EXPECT_TRUE(manifest->CoversCatalogLists(GetKoLockedFilterLists()));
}

TEST(KoFilterListManifestTest, RejectsMalformedManifests) {
  const int64_t v = 202611260000;
  auto with = [&](auto mutate) {
    auto lists = CoreLists("a");
    mutate(lists);
    return KoFilterListManifest::Parse(BuildManifest(lists, v)).has_value();
  };
  EXPECT_FALSE(with([](auto& l) { l[0].id = "../evil"; }));
  EXPECT_FALSE(with([](auto& l) { l[0].id = "Upper"; }));
  EXPECT_FALSE(with([](auto& l) { l[0].id = "-dash"; }));
  EXPECT_FALSE(with([](auto& l) { l[1].id = l[0].id; }));
  EXPECT_FALSE(with([](auto& l) { l[0].engine = "everything"; }));
  EXPECT_FALSE(with([](auto& l) { l[0].kind = "script"; }));
  EXPECT_FALSE(with([](auto& l) { l[0].replaces = "bad uuid"; }));
  EXPECT_FALSE(with([](auto& l) { l[0].content.clear(); }));
  // Over what the update channel can download.
  EXPECT_FALSE(
      with([](auto& l) { l[0].content.assign(kKoMaxListBytes + 1, 'a'); }));
  EXPECT_TRUE(with([](auto& l) { l[0].content.assign(kKoMaxListBytes, 'a'); }));

  EXPECT_FALSE(KoFilterListManifest::Parse("").has_value());
  EXPECT_FALSE(KoFilterListManifest::Parse("[]").has_value());
  EXPECT_FALSE(KoFilterListManifest::Parse(BuildManifest(CoreLists("a"), -1))
                   .has_value());

  std::string json = BuildManifest(CoreLists("a"), v);
  std::string wrong_channel = json;
  wrong_channel.replace(wrong_channel.find(kKoFilterListChannel),
                        strlen(kKoFilterListChannel), "other-channel");
  EXPECT_FALSE(KoFilterListManifest::Parse(wrong_channel).has_value());

  std::string traversal = json;
  traversal.replace(traversal.find("ko-extra.txt"), strlen("ko-extra.txt"),
                    "../ko-extra");
  EXPECT_FALSE(KoFilterListManifest::Parse(traversal).has_value());
}

TEST(KoFilterListManifestTest, VerifiesSignatures) {
  const std::string manifest = "{\"schema\":1}\n";
  // Compatible with publish_lists.py.
  EXPECT_TRUE(
      VerifyKoManifestSignature(manifest, kPythonSignatureHex, kPublicKeyHex));
  EXPECT_TRUE(VerifyKoManifestSignature(
      manifest, std::string(kPythonSignatureHex) + "\n", kPublicKeyHex));
  EXPECT_EQ(kPythonSignatureHex, Sign(manifest));

  EXPECT_FALSE(VerifyKoManifestSignature(manifest + " ", kPythonSignatureHex,
                                         kPublicKeyHex));
  std::string flipped = kPythonSignatureHex;
  flipped[0] = flipped[0] == '0' ? '1' : '0';
  EXPECT_FALSE(VerifyKoManifestSignature(manifest, flipped, kPublicKeyHex));
  EXPECT_FALSE(VerifyKoManifestSignature(manifest, "", kPublicKeyHex));
  EXPECT_FALSE(VerifyKoManifestSignature(manifest, kPythonSignatureHex, ""));
  EXPECT_FALSE(VerifyKoManifestSignature(manifest, kPythonSignatureHex, "zz"));
  std::string other_key = kPublicKeyHex;
  other_key[63] = other_key[63] == 'a' ? 'b' : 'a';
  EXPECT_FALSE(
      VerifyKoManifestSignature(manifest, kPythonSignatureHex, other_key));

  EXPECT_TRUE(IsValidKoPublicKey(kPublicKeyHex));
  EXPECT_FALSE(IsValidKoPublicKey(""));
  EXPECT_FALSE(IsValidKoPublicKey(std::string(kPublicKeyHex) + "00"));
}

TEST(KoFilterListManifestTest, ChecksDigests) {
  KoFilterListEntry entry;
  entry.size = 4;
  entry.sha256 = Sha256Hex("data");
  EXPECT_TRUE(KoListMatchesDigest("data", entry));
  EXPECT_FALSE(KoListMatchesDigest("date", entry));
  EXPECT_FALSE(KoListMatchesDigest("data!", entry));
}

TEST(KoFilterListManifestTest, FlagsUnsafeExceptions) {
  for (const char* rule : {"@@*", "@@||*^", "@@|https://", "@@^$document",
                           "@@*$ghide", "@@*$domain=~example.com", "@@/.*/"}) {
    EXPECT_TRUE(KoListHasUnsafeException(std::string("||a.example^\n") + rule))
        << rule;
  }
  EXPECT_FALSE(KoListHasUnsafeException(
      "@@*$ghide,domain=example.com\n@@||example.com^\n@@/cdn-cgi/access/\n"
      "@@*$to=example.org\n##.ad\n"));
}

// Snapshot loader and provider.

class KoFiltersProviderTest : public testing::Test {
 protected:
  base::test::TaskEnvironment task_environment_;
};

TEST_F(KoFiltersProviderTest, SelectsBaselineListsPerEngine) {
  const auto lists = CoreLists("embedded");
  KoFilterListSource source(BuildManifest(lists, 100), LoaderFor(lists),
                            base::FilePath(), "");
  ASSERT_TRUE(source.IsReady());

  std::set<std::string> ready_components;
  std::set<std::string> disabled_lists;
  auto enabled = base::BindLambdaForTesting(
      [&](const std::string& uuid) { return !disabled_lists.contains(uuid); });
  auto ready = base::BindLambdaForTesting(
      [&](const std::string& uuid) { return ready_components.contains(uuid); });

  AdBlockFiltersProviderManager manager(false);
  KoFiltersProvider default_engine(&source, &manager, true, enabled, ready);
  KoFiltersProvider additional_engine(&source, &manager, false, enabled, ready);
  EXPECT_TRUE(default_engine.IsInitialized());

  EXPECT_EQ((std::vector<std::string>{"ko-default", "ko-privacy", "ko-extra"}),
            Ids(default_engine.GetSelectedListsForTesting()));
  EXPECT_EQ(
      (std::vector<std::string>{"ko-firstparty", "ko-cookie", "ko-extra"}),
      Ids(additional_engine.GetSelectedListsForTesting()));

  // Component data supersedes the snapshot of the same catalog entry.
  ready_components.insert(kDefaultAdblockFiltersListUuid);
  EXPECT_EQ((std::vector<std::string>{"ko-privacy", "ko-extra"}),
            Ids(default_engine.GetSelectedListsForTesting()));

  // Disabled catalog entries are not loaded; K-O extras always are.
  disabled_lists.insert(kCookieListUuid);
  EXPECT_EQ((std::vector<std::string>{"ko-firstparty", "ko-extra"}),
            Ids(additional_engine.GetSelectedListsForTesting()));
}

TEST_F(KoFiltersProviderTest, LoadsVerifiedListsIntoFilterSet) {
  auto lists = CoreLists("embedded");
  // The loader returns tampered data for one list; it must be dropped.
  auto served = lists;
  served[1].content = "@@*\n";
  int loads = 0;
  auto inner = LoaderFor(served);
  KoFilterListSource source(
      BuildManifest(lists, 100),
      base::BindLambdaForTesting([&loads, inner](const std::string& id) {
        ++loads;
        return inner.Run(id);
      }),
      base::FilePath(), "");

  AdBlockFiltersProviderManager manager(false);
  auto all = base::BindRepeating([](const std::string&) { return true; });
  auto none = base::BindRepeating([](const std::string&) { return false; });
  KoFiltersProvider provider(&source, &manager, true, all, none);

  base::OnceCallback<void(rust::Box<adblock::FilterSet>*)> add_lists;
  provider.LoadFilterSet(base::BindLambdaForTesting(
      [&](base::OnceCallback<void(rust::Box<adblock::FilterSet>*)> cb) {
        add_lists = std::move(cb);
      }));
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(add_lists);
  EXPECT_EQ(3, loads);

  auto filter_set = adblock::new_filter_set(false);
  std::move(add_lists).Run(&filter_set);
}

TEST_F(KoFiltersProviderTest, NotifiesWhenRefreshed) {
  const auto lists = CoreLists("embedded");
  KoFilterListSource source(BuildManifest(lists, 100), LoaderFor(lists),
                            base::FilePath(), "");

  class Observer : public AdBlockFiltersProvider::Observer {
   public:
    void OnChanged(bool is_default_engine) override { ++changes; }
    int changes = 0;
  } observer;

  AdBlockFiltersProviderManager manager(false);
  auto all = base::BindRepeating([](const std::string&) { return true; });
  KoFiltersProvider provider(&source, &manager, true, all, all);
  provider.AddObserver(&observer);
  provider.Refresh();
  EXPECT_EQ(1, observer.changes);
  provider.RemoveObserver(&observer);
}

// Stored channel snapshots.

class KoFilterListSourceTest : public testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(temp_dir_.CreateUniqueTempDir()); }
  base::FilePath remote_dir() const {
    return temp_dir_.GetPath().AppendASCII("ko_filter_lists");
  }

  base::test::TaskEnvironment task_environment_;
  base::ScopedTempDir temp_dir_;
};

TEST_F(KoFilterListSourceTest, UsesNewerSignedRemoteSet) {
  const auto embedded = CoreLists("embedded");
  const auto remote = CoreLists("remote");
  const std::string manifest = BuildManifest(remote, 200);
  WriteRemoteSet(remote_dir(), remote, manifest, Sign(manifest));

  KoFilterListSource source(BuildManifest(embedded, 100), LoaderFor(embedded),
                            remote_dir(), kPublicKeyHex);
  EXPECT_FALSE(source.IsReady());
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(source.IsReady());
  EXPECT_TRUE(source.is_remote_active());
  EXPECT_EQ(200, source.manifest().version);

  std::optional<std::string> data;
  source.LoadList(*source.manifest().Find("ko-default"),
                  base::BindLambdaForTesting(
                      [&](std::optional<std::string> d) { data = d; }));
  task_environment_.RunUntilIdle();
  EXPECT_EQ(remote[0].content, data);
}

TEST_F(KoFilterListSourceTest, RejectsTamperedOrStaleRemoteSets) {
  const auto embedded = CoreLists("embedded");
  auto remote = CoreLists("remote");
  const std::string manifest = BuildManifest(remote, 200);

  // A file changed after signing.
  auto tampered = remote;
  tampered[0].content = "@@||ads-remote.example^\n";
  WriteRemoteSet(remote_dir(), tampered, manifest, Sign(manifest));
  EXPECT_FALSE(LoadVerifiedKoRemoteSet(remote_dir(), kPublicKeyHex, 100));

  // Bad signature.
  ASSERT_TRUE(base::DeletePathRecursively(remote_dir()));
  WriteRemoteSet(remote_dir(), remote, manifest, Sign(manifest + " "));
  EXPECT_FALSE(LoadVerifiedKoRemoteSet(remote_dir(), kPublicKeyHex, 100));

  // Not newer than the embedded snapshot.
  ASSERT_TRUE(base::DeletePathRecursively(remote_dir()));
  WriteRemoteSet(remote_dir(), remote, manifest, Sign(manifest));
  EXPECT_FALSE(LoadVerifiedKoRemoteSet(remote_dir(), kPublicKeyHex, 200));
  EXPECT_TRUE(LoadVerifiedKoRemoteSet(remote_dir(), kPublicKeyHex, 100));

  // Missing a locked list.
  ASSERT_TRUE(base::DeletePathRecursively(remote_dir()));
  remote.erase(remote.begin());
  const std::string partial = BuildManifest(remote, 300);
  WriteRemoteSet(remote_dir(), remote, partial, Sign(partial));
  EXPECT_FALSE(LoadVerifiedKoRemoteSet(remote_dir(), kPublicKeyHex, 100));

  // The source falls back to the embedded set and removes bad data.
  KoFilterListSource source(BuildManifest(embedded, 100), LoaderFor(embedded),
                            remote_dir(), kPublicKeyHex);
  task_environment_.RunUntilIdle();
  EXPECT_TRUE(source.IsReady());
  EXPECT_FALSE(source.is_remote_active());
  EXPECT_EQ(100, source.manifest().version);
  EXPECT_FALSE(base::PathExists(remote_dir()));
}

TEST_F(KoFilterListSourceTest, DropsRemoteSetCorruptedAfterLoad) {
  const auto embedded = CoreLists("embedded");
  const auto remote = CoreLists("remote");
  const std::string manifest = BuildManifest(remote, 200);
  WriteRemoteSet(remote_dir(), remote, manifest, Sign(manifest));
  KoFilterListSource source(BuildManifest(embedded, 100), LoaderFor(embedded),
                            remote_dir(), kPublicKeyHex);
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(source.is_remote_active());

  ASSERT_TRUE(
      base::WriteFile(remote_dir().AppendASCII("ko-default.txt"), "@@*\n"));
  std::optional<std::string> data = "unset";
  source.LoadList(*source.manifest().Find("ko-default"),
                  base::BindLambdaForTesting(
                      [&](std::optional<std::string> d) { data = d; }));
  task_environment_.RunUntilIdle();
  EXPECT_FALSE(data);
  EXPECT_FALSE(source.is_remote_active());
  EXPECT_EQ(100, source.manifest().version);
}

// Update channel.

class KoFilterListUpdaterTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    KoFilterListUpdater::RegisterLocalStatePrefs(local_state_.registry());
    embedded_ = CoreLists("embedded");
    source_ = std::make_unique<KoFilterListSource>(
        BuildManifest(embedded_, 100), LoaderFor(embedded_),
        temp_dir_.GetPath().AppendASCII("ko_filter_lists"), kPublicKeyHex);
    task_environment_.RunUntilIdle();
    updater_ = std::make_unique<KoFilterListUpdater>(
        source_.get(),
        base::MakeRefCounted<network::WeakWrapperSharedURLLoaderFactory>(
            &url_loader_factory_),
        &local_state_, kChannelUrl,
        base::BindLambdaForTesting([&] { ++installs_; }));
  }

  void Serve(const std::vector<TestList>& lists,
             const std::string& manifest,
             const std::string& signature) {
    url_loader_factory_.AddResponse(std::string(kChannelUrl) + "manifest.json",
                                    manifest);
    url_loader_factory_.AddResponse(
        std::string(kChannelUrl) + "manifest.json.sig", signature);
    for (const auto& list : lists) {
      url_loader_factory_.AddResponse(
          std::string(kChannelUrl) + list.id + ".txt", list.content);
    }
  }

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  base::ScopedTempDir temp_dir_;
  TestingPrefServiceSimple local_state_;
  network::TestURLLoaderFactory url_loader_factory_;
  std::vector<TestList> embedded_;
  std::unique_ptr<KoFilterListSource> source_;
  std::unique_ptr<KoFilterListUpdater> updater_;
  int installs_ = 0;
};

TEST_F(KoFilterListUpdaterTest, InstallsVerifiedSnapshot) {
  const auto remote = CoreLists("remote");
  const std::string manifest = BuildManifest(remote, 200);
  Serve(remote, manifest, Sign(manifest));

  updater_->CheckNow();
  task_environment_.RunUntilIdle();
  EXPECT_EQ(1, installs_);
  EXPECT_TRUE(source_->is_remote_active());
  EXPECT_EQ(200, source_->manifest().version);
  EXPECT_EQ("200", local_state_.GetString(kKoFilterListsAcceptedVersion));
  // What was installed verifies on the next start.
  EXPECT_TRUE(
      LoadVerifiedKoRemoteSet(source_->remote_dir(), kPublicKeyHex, 100));
}

TEST_F(KoFilterListUpdaterTest, RejectsBadSignature) {
  const auto remote = CoreLists("remote");
  const std::string manifest = BuildManifest(remote, 200);
  Serve(remote, manifest, Sign("something else"));

  updater_->CheckNow();
  task_environment_.RunUntilIdle();
  EXPECT_EQ(0, installs_);
  EXPECT_FALSE(source_->is_remote_active());
  EXPECT_FALSE(base::PathExists(source_->remote_dir()));
}

TEST_F(KoFilterListUpdaterTest, RejectsListNotMatchingManifest) {
  auto remote = CoreLists("remote");
  const std::string manifest = BuildManifest(remote, 200);
  remote[2].content = "||other.example^\n";
  Serve(remote, manifest, Sign(manifest));

  updater_->CheckNow();
  task_environment_.RunUntilIdle();
  EXPECT_EQ(0, installs_);
  EXPECT_FALSE(source_->is_remote_active());
}

TEST_F(KoFilterListUpdaterTest, IgnoresOlderSnapshots) {
  const auto remote = CoreLists("remote");
  const std::string manifest = BuildManifest(remote, 100);
  Serve(remote, manifest, Sign(manifest));

  updater_->CheckNow();
  task_environment_.RunUntilIdle();
  EXPECT_EQ(0, installs_);
  EXPECT_FALSE(source_->is_remote_active());
}

TEST(KoFilterListUpdaterStaticTest, AcceptsOnlyNewerVersions) {
  // version, embedded, accepted, remote_active, active
  EXPECT_TRUE(KoFilterListUpdater::ShouldAccept(200, 100, 0, false, 100));
  EXPECT_FALSE(KoFilterListUpdater::ShouldAccept(100, 100, 0, false, 100));
  EXPECT_FALSE(KoFilterListUpdater::ShouldAccept(200, 100, 200, true, 200));
  EXPECT_TRUE(KoFilterListUpdater::ShouldAccept(300, 100, 200, true, 200));
  // Replay of an older signed manifest.
  EXPECT_FALSE(KoFilterListUpdater::ShouldAccept(150, 100, 200, false, 100));
  // Restoring a lost copy of the last accepted version.
  EXPECT_TRUE(KoFilterListUpdater::ShouldAccept(200, 100, 200, false, 100));
}

TEST(KoFilterListUpdaterStaticTest, RequiresHttpsBaseUrl) {
  EXPECT_EQ(
      GURL("https://lists.example.com/k-o/"),
      KoFilterListUpdater::ValidateBaseUrl("https://lists.example.com/k-o"));
  EXPECT_FALSE(KoFilterListUpdater::ValidateBaseUrl("http://lists.example.com/")
                   .is_valid());
  EXPECT_FALSE(KoFilterListUpdater::ValidateBaseUrl(
                   "https://user:pass@lists.example.com/")
                   .is_valid());
  EXPECT_FALSE(
      KoFilterListUpdater::ValidateBaseUrl("https://lists.example.com/?a=b")
          .is_valid());
  EXPECT_FALSE(KoFilterListUpdater::ValidateBaseUrl("").is_valid());
}

}  // namespace brave_shields
