// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/containers/core/browser/container_proxy.h"

#include <string>

#include "base/test/scoped_feature_list.h"
#include "base/values.h"
#include "brave/components/containers/core/browser/prefs_registration.h"
#include "brave/components/containers/core/common/features.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/sync_preferences/testing_pref_service_syncable.h"
#include "net/base/host_port_pair.h"
#include "net/base/proxy_chain.h"
#include "net/base/proxy_server.h"
#include "net/proxy_resolution/proxy_config.h"
#include "net/proxy_resolution/proxy_config_with_annotation.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace containers {

namespace {

constexpr char kUser[] = "zone-user";
constexpr char kSecret[] = "s3cr3t-Pa55";

void ExpectNoSecrets(const std::string& text) {
  EXPECT_EQ(text.find(kUser), std::string::npos) << text;
  EXPECT_EQ(text.find(kSecret), std::string::npos) << text;
}

const net::ProxyServer& OnlyProxyServer(
    const net::ProxyConfigWithAnnotation& config) {
  const auto& rules = config.value().proxy_rules();
  EXPECT_EQ(rules.type, net::ProxyConfig::ProxyRules::Type::PROXY_LIST);
  EXPECT_EQ(rules.single_proxies.size(), 1u);
  const net::ProxyChain& chain = rules.single_proxies.First();
  EXPECT_FALSE(chain.is_direct());
  EXPECT_TRUE(chain.is_single_proxy());
  return chain.GetProxyServer(/*chain_index=*/0);
}

}  // namespace

TEST(ContainerProxyTest, ParsesServers) {
  const struct {
    const char* input;
    ContainerProxyScheme scheme;
    const char* host;
    uint16_t port;
    const char* server;
  } kCases[] = {
      {"socks5h://100.64.0.10:1080", ContainerProxyScheme::kSocks5h,
       "100.64.0.10", 1080, "socks5h://100.64.0.10:1080"},
      {"socks5://peer.example.com", ContainerProxyScheme::kSocks5,
       "peer.example.com", 1080, "socks5://peer.example.com:1080"},
      {"http://example.com", ContainerProxyScheme::kHttp, "example.com", 80,
       "http://example.com:80"},
      {"https://Proxy.Example.com/", ContainerProxyScheme::kHttps,
       "proxy.example.com", 443, "https://proxy.example.com:443"},
      {"  SOCKS5H://[fd00::1]:9050  ", ContainerProxyScheme::kSocks5h,
       "[fd00::1]", 9050, "socks5h://[fd00::1]:9050"},
      {"socks5h://100.64.0.10:80", ContainerProxyScheme::kSocks5h,
       "100.64.0.10", 80, "socks5h://100.64.0.10:80"},
  };
  for (const auto& test : kCases) {
    SCOPED_TRACE(test.input);
    auto parsed = ParseContainerProxyUrl(test.input);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->server.scheme, test.scheme);
    EXPECT_EQ(parsed->server.host, test.host);
    EXPECT_EQ(parsed->server.port, test.port);
    EXPECT_FALSE(parsed->credentials);
    EXPECT_EQ(parsed->server.ToString(), test.server);

    // The stored form parses back to the same server.
    auto reparsed = ParseContainerProxyServer(parsed->server.ToString());
    ASSERT_TRUE(reparsed.has_value());
    EXPECT_EQ(*reparsed, parsed->server);
  }
}

TEST(ContainerProxyTest, RejectsInvalidUrls) {
  const struct {
    const char* input;
    ContainerProxyError error;
  } kCases[] = {
      {"", ContainerProxyError::kEmpty},
      {"   ", ContainerProxyError::kEmpty},
      {"100.64.0.10:1080", ContainerProxyError::kInvalidUrl},
      {"://100.64.0.10", ContainerProxyError::kInvalidUrl},
      {"ftp://100.64.0.10", ContainerProxyError::kUnsupportedScheme},
      {"socks4://100.64.0.10", ContainerProxyError::kUnsupportedScheme},
      {"quic://100.64.0.10", ContainerProxyError::kUnsupportedScheme},
      {"socks5h://", ContainerProxyError::kInvalidUrl},
      {"socks5h://:1080", ContainerProxyError::kInvalidUrl},
      {"socks5h://ho st:1080", ContainerProxyError::kInvalidUrl},
      {"socks5h://[::1", ContainerProxyError::kInvalidUrl},
      {"socks5h://::1:1080", ContainerProxyError::kInvalidUrl},
      {"socks5h://host:0", ContainerProxyError::kInvalidPort},
      {"socks5h://host:65536", ContainerProxyError::kInvalidPort},
      {"socks5h://host:", ContainerProxyError::kInvalidPort},
      {"socks5h://host:12ab", ContainerProxyError::kInvalidPort},
      {"socks5h://host/path", ContainerProxyError::kUnexpectedComponents},
      {"socks5h://host?x=1", ContainerProxyError::kUnexpectedComponents},
      {"socks5h://host#frag", ContainerProxyError::kUnexpectedComponents},
  };
  for (const auto& test : kCases) {
    SCOPED_TRACE(test.input);
    auto parsed = ParseContainerProxyUrl(test.input);
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error(), test.error);
    EXPECT_FALSE(ContainerProxyErrorToString(parsed.error()).empty());
  }

  EXPECT_EQ(ParseContainerProxyUrl("socks5h://" + std::string(3000, 'a'))
                .error(),
            ContainerProxyError::kTooLong);
}

TEST(ContainerProxyTest, ParsesCredentials) {
  auto parsed = ParseContainerProxyUrl(
      "socks5h://zone-user:s3cr3t-Pa55%40%3A@100.64.0.10:1080");
  ASSERT_TRUE(parsed.has_value());
  ASSERT_TRUE(parsed->credentials);
  EXPECT_EQ(parsed->credentials->username, kUser);
  EXPECT_EQ(parsed->credentials->password, std::string(kSecret) + "@:");
  EXPECT_EQ(parsed->server.ToString(), "socks5h://100.64.0.10:1080");

  // Username only.
  parsed = ParseContainerProxyUrl("socks5://zone-user@10.0.0.2");
  ASSERT_TRUE(parsed.has_value());
  ASSERT_TRUE(parsed->credentials);
  EXPECT_EQ(parsed->credentials->username, kUser);
  EXPECT_TRUE(parsed->credentials->password.empty());

  // Private host names are allowed for cleartext credentials.
  EXPECT_TRUE(
      ParseContainerProxyUrl("socks5h://u:p@peer.netbird.cloud:1080")
          .has_value());
  EXPECT_TRUE(ParseContainerProxyUrl("http://u:p@localhost:8080").has_value());
  EXPECT_TRUE(ParseContainerProxyUrl("http://u:p@[fd00::5]:8080").has_value());
  // TLS to the proxy allows any host.
  EXPECT_TRUE(
      ParseContainerProxyUrl("https://u:p@proxy.example.com").has_value());
}

TEST(ContainerProxyTest, RejectsBadCredentials) {
  const struct {
    const char* input;
    ContainerProxyError error;
  } kCases[] = {
      {"socks5h://:pass@10.0.0.1", ContainerProxyError::kInvalidCredentials},
      {"socks5h://us%zzer:p@10.0.0.1",
       ContainerProxyError::kInvalidCredentials},
      {"socks5h://user:pa%0Ass@10.0.0.1",
       ContainerProxyError::kInvalidCredentials},
      {"socks5h://us%20er:p@10.0.0.1",
       ContainerProxyError::kInvalidCredentials},
      {"socks5h://user:%00@10.0.0.1", ContainerProxyError::kInvalidCredentials},
      // HTTP Basic can't carry ':' in the username.
      {"http://us%3Aer:p@10.0.0.1", ContainerProxyError::kInvalidCredentials},
      // Cleartext credentials to public hosts.
      {"socks5h://u:p@proxy.example.com",
       ContainerProxyError::kInsecureCredentials},
      {"http://u:p@8.8.8.8:3128", ContainerProxyError::kInsecureCredentials},
      {"socks5://u:p@[2001:4860:4860::8888]",
       ContainerProxyError::kInsecureCredentials},
  };
  for (const auto& test : kCases) {
    SCOPED_TRACE(test.input);
    auto parsed = ParseContainerProxyUrl(test.input);
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error(), test.error);
  }

  EXPECT_TRUE(
      ParseContainerProxyUrl("socks5://us%3Aer:p@10.0.0.1").has_value());

  const std::string long_user(256, 'u');
  EXPECT_EQ(ParseContainerProxyUrl("socks5h://" + long_user + ":p@10.0.0.1")
                .error(),
            ContainerProxyError::kInvalidCredentials);
}

TEST(ContainerProxyTest, StoredServerRejectsCredentials) {
  auto server = ParseContainerProxyServer("socks5h://u:p@10.0.0.1:1080");
  ASSERT_FALSE(server.has_value());
  EXPECT_EQ(server.error(), ContainerProxyError::kInvalidCredentials);
}

TEST(ContainerProxyTest, ToStringNeverContainsCredentials) {
  auto parsed = ParseContainerProxyUrl(
      "socks5h://zone-user:s3cr3t-Pa55@100.64.0.10:1080");
  ASSERT_TRUE(parsed.has_value());
  ASSERT_TRUE(parsed->credentials);

  ExpectNoSecrets(parsed->ToString());
  ExpectNoSecrets(parsed->server.ToString());
  ExpectNoSecrets(parsed->credentials->ToString());
  EXPECT_EQ(parsed->ToString(),
            "socks5h://100.64.0.10:1080 (credentials <redacted>)");

  // Errors carry no input either.
  auto error = ParseContainerProxyUrl("socks5h://zone-user:s3cr3t-Pa55@[::1");
  ASSERT_FALSE(error.has_value());
  ExpectNoSecrets(std::string(ContainerProxyErrorToString(error.error())));
}

TEST(ContainerProxyTest, CredentialsSerializationRoundTrip) {
  const ContainerProxyCredentials credentials(kUser, "p\"a:s@s\\");
  const std::string serialized =
      SerializeContainerProxyCredentials(credentials);
  auto restored = DeserializeContainerProxyCredentials(serialized);
  ASSERT_TRUE(restored);
  EXPECT_EQ(*restored, credentials);

  EXPECT_FALSE(DeserializeContainerProxyCredentials(""));
  EXPECT_FALSE(DeserializeContainerProxyCredentials("not json"));
  EXPECT_FALSE(DeserializeContainerProxyCredentials("{\"p\":\"x\"}"));
  EXPECT_FALSE(DeserializeContainerProxyCredentials("{\"u\":\"\",\"p\":\"\"}"));
}

TEST(ContainerProxyTest, SocksConfigCarriesCredentialsWithoutFallback) {
  auto parsed = ParseContainerProxyUrl(
      "socks5h://zone-user:s3cr3t-Pa55@100.64.0.10:1080");
  ASSERT_TRUE(parsed.has_value());

  const auto config =
      CreateContainerProxyConfig(parsed->server, &*parsed->credentials);
  const net::ProxyServer& server = OnlyProxyServer(config);
  EXPECT_EQ(server.scheme(), net::ProxyServer::SCHEME_SOCKS5);
  EXPECT_EQ(server.host_port_pair().host(), "100.64.0.10");
  EXPECT_EQ(server.host_port_pair().port(), 1080);
  EXPECT_EQ(server.host_port_pair().username(), kUser);
  EXPECT_EQ(server.host_port_pair().password(), kSecret);
  EXPECT_FALSE(config.value().HasAutomaticSettings());
}

TEST(ContainerProxyTest, HttpConfigKeepsCredentialsOutOfServer) {
  auto parsed = ParseContainerProxyUrl("http://zone-user:s3cr3t-Pa55@[fd00::5]");
  ASSERT_TRUE(parsed.has_value());

  const auto config =
      CreateContainerProxyConfig(parsed->server, &*parsed->credentials);
  const net::ProxyServer& server = OnlyProxyServer(config);
  EXPECT_EQ(server.scheme(), net::ProxyServer::SCHEME_HTTP);
  EXPECT_EQ(server.host_port_pair().host(), "fd00::5");
  EXPECT_EQ(server.host_port_pair().port(), 80);
  EXPECT_TRUE(server.host_port_pair().username().empty());
  EXPECT_TRUE(server.host_port_pair().password().empty());
}

TEST(ContainerProxyTest, FailClosedConfigHasNoDirectFallback) {
  const auto config = CreateFailClosedContainerProxyConfig();
  const net::ProxyServer& server = OnlyProxyServer(config);
  EXPECT_EQ(server.host_port_pair().host(), "127.0.0.1");
  EXPECT_EQ(server.host_port_pair().port(), 9);
}

class ContainerProxyPrefsTest : public testing::Test {
 protected:
  void SetUp() override {
    feature_list_.InitAndEnableFeature(features::kContainers);
    RegisterProfilePrefs(prefs_.registry());
  }

  base::test::ScopedFeatureList feature_list_;
  sync_preferences::TestingPrefServiceSyncable prefs_;
};

TEST_F(ContainerProxyPrefsTest, StoresServerAndCiphertextOnly) {
  EXPECT_FALSE(GetStoredContainerProxy(prefs_, "c1"));
  EXPECT_TRUE(GetContainerIdsWithStoredProxy(prefs_).empty());

  SetStoredContainerProxy(prefs_, "c1",
                          {.server = "socks5h://100.64.0.10:1080",
                           .encrypted_credentials = "Y2lwaGVy"});
  SetStoredContainerProxy(prefs_, "c2", {.server = "http://10.0.0.2:3128"});

  auto c1 = GetStoredContainerProxy(prefs_, "c1");
  ASSERT_TRUE(c1);
  EXPECT_EQ(c1->server, "socks5h://100.64.0.10:1080");
  EXPECT_EQ(c1->encrypted_credentials, "Y2lwaGVy");
  auto c2 = GetStoredContainerProxy(prefs_, "c2");
  ASSERT_TRUE(c2);
  EXPECT_TRUE(c2->encrypted_credentials.empty());

  const base::DictValue& dict = prefs_.GetDict(kKoContainerProxiesPref);
  const base::DictValue* entry = dict.FindDict("c1");
  ASSERT_TRUE(entry);
  EXPECT_EQ(entry->size(), 2u);
  EXPECT_FALSE(dict.FindDict("c2")->contains("credentials"));
  EXPECT_EQ(GetContainerIdsWithStoredProxy(prefs_).size(), 2u);

  // The proxies pref is local only.
  EXPECT_EQ(prefs_.registry()->GetRegistrationFlags(kKoContainerProxiesPref) &
                user_prefs::PrefRegistrySyncable::SYNCABLE_PREF,
            0u);

  RemoveStoredContainerProxy(prefs_, "c1");
  EXPECT_FALSE(GetStoredContainerProxy(prefs_, "c1"));
  RemoveStoredContainerProxy(prefs_, "missing");
  EXPECT_EQ(GetContainerIdsWithStoredProxy(prefs_).size(), 1u);
}

}  // namespace containers
