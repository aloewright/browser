// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/browser/containers/ko_container_proxy_service.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/run_loop.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "brave/components/containers/core/browser/container_proxy.h"
#include "brave/components/containers/core/browser/prefs_registration.h"
#include "brave/components/containers/core/common/features.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/browser/test_utils.h"
#include "components/sync_preferences/testing_pref_service_syncable.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/base/host_port_pair.h"
#include "net/base/proxy_chain.h"
#include "net/base/proxy_server.h"
#include "net/proxy_resolution/proxy_config.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/scheme_host_port.h"

namespace containers {

namespace {

constexpr char kContainerId[] = "0c8d7b4e-5d0f-4b1a-9a52-6f1d2c3b4a59";
constexpr char kSecret[] = "s3cr3t-Pa55";

class FakeProxyConfigClient : public network::mojom::ProxyConfigClient {
 public:
  explicit FakeProxyConfigClient(
      mojo::PendingReceiver<network::mojom::ProxyConfigClient> receiver)
      : receiver_(this, std::move(receiver)) {}

  void OnProxyConfigUpdated(
      const net::ProxyConfigWithAnnotation& proxy_config) override {
    configs_.push_back(proxy_config);
  }
  void FlushProxyConfig(FlushProxyConfigCallback callback) override {
    std::move(callback).Run();
  }

  void Flush() { receiver_.FlushForTesting(); }
  const std::vector<net::ProxyConfigWithAnnotation>& configs() const {
    return configs_;
  }

 private:
  mojo::Receiver<network::mojom::ProxyConfigClient> receiver_;
  std::vector<net::ProxyConfigWithAnnotation> configs_;
};

const net::ProxyServer& OnlyServer(const net::ProxyConfigWithAnnotation& c) {
  const auto& rules = c.value().proxy_rules();
  EXPECT_EQ(rules.type, net::ProxyConfig::ProxyRules::Type::PROXY_LIST);
  EXPECT_EQ(rules.single_proxies.size(), 1u);
  return rules.single_proxies.First().GetProxyServer(/*chain_index=*/0);
}

}  // namespace

class KoContainerProxyServiceTest : public testing::Test {
 protected:
  void SetUp() override {
    feature_list_.InitAndEnableFeature(features::kContainers);
    RegisterProfilePrefs(prefs_.registry());
    os_crypt_ = os_crypt_async::GetTestOSCryptAsyncForTesting(
        /*is_sync_for_unittests=*/true);
    service_ = CreateService(os_crypt_.get());
  }

  void TearDown() override { service_.reset(); }

  std::unique_ptr<KoContainerProxyService> CreateService(
      os_crypt_async::OSCryptAsync* os_crypt) {
    return std::make_unique<KoContainerProxyService>(
        &prefs_, os_crypt,
        base::BindRepeating(
            [](const KoContainerProxyServiceTest* test, std::string_view id) {
              return test->container_exists_;
            },
            base::Unretained(this)),
        base::NullCallback());
  }

  bool SetProxy(const std::string& url, bool keep_credentials = false) {
    auto parsed = ParseContainerProxyUrl(url);
    EXPECT_TRUE(parsed.has_value());
    base::test::TestFuture<bool> future;
    service_->SetProxy(kContainerId, std::move(*parsed), keep_credentials,
                       future.GetCallback());
    return future.Get();
  }

  // Mimics the profile's ProxyConfigMonitor: an initial config plus a client
  // pipe for later updates, which `profile_monitor_` drives.
  network::mojom::NetworkContextParamsPtr ConfigureContainerContext() {
    auto params = network::mojom::NetworkContextParams::New();
    params->initial_proxy_config =
        net::ProxyConfigWithAnnotation::CreateDirect();
    profile_monitor_.reset();
    params->proxy_config_client_receiver =
        profile_monitor_.BindNewPipeAndPassReceiver();
    service_->ConfigureNetworkContextParams(
        KoContainerProxyService::GetRelativePartitionPath(kContainerId),
        params.get());
    return params;
  }

  void RemoveProxy() {
    base::test::TestFuture<bool> removed;
    service_->SetProxy(kContainerId, std::nullopt, false,
                       removed.GetCallback());
    ASSERT_TRUE(removed.Get());
  }

  base::test::TaskEnvironment task_environment_;
  base::test::ScopedFeatureList feature_list_;
  sync_preferences::TestingPrefServiceSyncable prefs_;
  std::unique_ptr<os_crypt_async::OSCryptAsync> os_crypt_;
  std::unique_ptr<KoContainerProxyService> service_;
  mojo::Remote<network::mojom::ProxyConfigClient> profile_monitor_;
  bool container_exists_ = true;
};

TEST_F(KoContainerProxyServiceTest, RelativePartitionPathMatchesContent) {
  EXPECT_EQ(KoContainerProxyService::GetRelativePartitionPath(kContainerId),
            base::FilePath(FILE_PATH_LITERAL("Storage"))
                .Append(FILE_PATH_LITERAL("ext"))
                .Append(FILE_PATH_LITERAL("containers"))
                .Append(FILE_PATH_LITERAL("4BC60C1AEEB7")));
}

TEST_F(KoContainerProxyServiceTest, OtherPartitionsAreUntouched) {
  ASSERT_TRUE(SetProxy("socks5h://100.64.0.10:1080"));
  auto params = network::mojom::NetworkContextParams::New();
  params->initial_proxy_config = net::ProxyConfigWithAnnotation::CreateDirect();
  service_->ConfigureNetworkContextParams(base::FilePath(), params.get());
  ASSERT_TRUE(params->initial_proxy_config);
  EXPECT_TRUE(params->initial_proxy_config->value().proxy_rules().empty());
  EXPECT_FALSE(params->proxy_config_client_receiver.is_valid());
}

TEST_F(KoContainerProxyServiceTest, ProxiedContainerGetsOwnConfig) {
  ASSERT_TRUE(SetProxy("socks5h://100.64.0.10:1080"));
  auto params = ConfigureContainerContext();
  ASSERT_TRUE(params->initial_proxy_config);
  const auto& server = OnlyServer(*params->initial_proxy_config);
  EXPECT_EQ(server.scheme(), net::ProxyServer::SCHEME_SOCKS5);
  EXPECT_EQ(server.host_port_pair().host(), "100.64.0.10");
  EXPECT_EQ(server.host_port_pair().port(), 1080);
  EXPECT_TRUE(params->proxy_config_client_receiver.is_valid());
}

TEST_F(KoContainerProxyServiceTest, CredentialsAreEncryptedAtRest) {
  ASSERT_TRUE(SetProxy(std::string("socks5h://zone-user:") + kSecret +
                       "@100.64.0.10:1080"));

  auto stored = GetStoredContainerProxy(prefs_, kContainerId);
  ASSERT_TRUE(stored);
  EXPECT_EQ(stored->server, "socks5h://100.64.0.10:1080");
  ASSERT_FALSE(stored->encrypted_credentials.empty());
  EXPECT_EQ(stored->encrypted_credentials.find(kSecret), std::string::npos);
  EXPECT_EQ(stored->encrypted_credentials.find("zone-user"), std::string::npos);

  auto ui = service_->GetProxyForUI(kContainerId);
  ASSERT_TRUE(ui);
  EXPECT_EQ(ui->url, "socks5h://100.64.0.10:1080");
  EXPECT_TRUE(ui->has_credentials);

  // SOCKS credentials are decrypted into the network context config only.
  auto params = ConfigureContainerContext();
  ASSERT_TRUE(params->initial_proxy_config);
  const auto& server = OnlyServer(*params->initial_proxy_config);
  EXPECT_EQ(server.host_port_pair().username(), "zone-user");
  EXPECT_EQ(server.host_port_pair().password(), kSecret);
}

TEST_F(KoContainerProxyServiceTest, KeepCredentialsOnlyForSameServer) {
  ASSERT_TRUE(SetProxy("socks5h://u:p@100.64.0.10:1080"));
  ASSERT_TRUE(SetProxy("socks5h://100.64.0.10:1080", /*keep=*/true));
  EXPECT_TRUE(service_->GetProxyForUI(kContainerId)->has_credentials);

  ASSERT_TRUE(SetProxy("socks5h://100.64.0.11:1080", /*keep=*/true));
  EXPECT_FALSE(service_->GetProxyForUI(kContainerId)->has_credentials);
}

TEST_F(KoContainerProxyServiceTest, HttpProxyCredentialsOnlyForThatProxy) {
  ASSERT_TRUE(
      SetProxy(std::string("http://zone-user:") + kSecret + "@10.0.0.2:3128"));

  // HTTP credentials never go into the proxy server itself.
  auto params = ConfigureContainerContext();
  const auto& server = OnlyServer(*params->initial_proxy_config);
  EXPECT_TRUE(server.host_port_pair().username().empty());

  auto credentials = service_->GetHttpProxyCredentials(
      kContainerId, url::SchemeHostPort("http", "10.0.0.2", 3128));
  ASSERT_TRUE(credentials);
  EXPECT_EQ(credentials->username(), u"zone-user");
  EXPECT_EQ(credentials->password(), base::UTF8ToUTF16(kSecret));

  EXPECT_FALSE(service_->GetHttpProxyCredentials(
      kContainerId, url::SchemeHostPort("http", "10.0.0.3", 3128)));
  EXPECT_FALSE(service_->GetHttpProxyCredentials(
      kContainerId, url::SchemeHostPort("http", "10.0.0.2", 8080)));
  EXPECT_FALSE(service_->GetHttpProxyCredentials(
      kContainerId, url::SchemeHostPort("https", "10.0.0.2", 3128)));
  EXPECT_FALSE(service_->GetHttpProxyCredentials(
      "other", url::SchemeHostPort("http", "10.0.0.2", 3128)));
}

TEST_F(KoContainerProxyServiceTest, LiveContextsFollowChanges) {
  ASSERT_TRUE(SetProxy("socks5h://100.64.0.10:1080"));
  auto params = ConfigureContainerContext();
  FakeProxyConfigClient client(std::move(params->proxy_config_client_receiver));

  ASSERT_TRUE(SetProxy("socks5h://100.64.0.11:1080"));
  client.Flush();
  ASSERT_EQ(client.configs().size(), 1u);
  EXPECT_EQ(OnlyServer(client.configs().back()).host_port_pair().host(),
            "100.64.0.11");

  // Profile-wide changes cannot redirect a proxied container.
  net::ProxyConfig profile_proxy;
  profile_proxy.proxy_rules().ParseFromString("http://10.0.0.7:3128");
  profile_monitor_->OnProxyConfigUpdated(net::ProxyConfigWithAnnotation(
      profile_proxy, TRAFFIC_ANNOTATION_FOR_TESTS));
  profile_monitor_.FlushForTesting();
  client.Flush();
  ASSERT_EQ(client.configs().size(), 1u);

  // Removing the proxy hands the context back to the profile's config.
  RemoveProxy();
  client.Flush();
  ASSERT_EQ(client.configs().size(), 2u);
  EXPECT_EQ(OnlyServer(client.configs().back()).host_port_pair().host(),
            "10.0.0.7");
}

TEST_F(KoContainerProxyServiceTest, ProxyAddedToLiveContextApplies) {
  // The container's context exists before it gets a proxy.
  auto params = ConfigureContainerContext();
  ASSERT_TRUE(params->initial_proxy_config);
  EXPECT_TRUE(params->initial_proxy_config->value().proxy_rules().empty());
  FakeProxyConfigClient client(std::move(params->proxy_config_client_receiver));

  // Unproxied, it follows the profile.
  net::ProxyConfig profile_proxy;
  profile_proxy.proxy_rules().ParseFromString("http://10.0.0.7:3128");
  profile_monitor_->OnProxyConfigUpdated(net::ProxyConfigWithAnnotation(
      profile_proxy, TRAFFIC_ANNOTATION_FOR_TESTS));
  profile_monitor_.FlushForTesting();
  client.Flush();
  ASSERT_EQ(client.configs().size(), 1u);
  EXPECT_EQ(OnlyServer(client.configs().back()).host_port_pair().host(),
            "10.0.0.7");

  ASSERT_TRUE(SetProxy("socks5h://100.64.0.10:1080"));
  client.Flush();
  ASSERT_EQ(client.configs().size(), 2u);
  EXPECT_EQ(OnlyServer(client.configs().back()).host_port_pair().host(),
            "100.64.0.10");
}

TEST_F(KoContainerProxyServiceTest, DeletedContainerStaysClosed) {
  ASSERT_TRUE(SetProxy("socks5h://100.64.0.10:1080"));
  auto params = ConfigureContainerContext();
  FakeProxyConfigClient client(std::move(params->proxy_config_client_receiver));

  container_exists_ = false;
  RemoveProxy();
  client.Flush();
  ASSERT_FALSE(client.configs().empty());
  EXPECT_EQ(OnlyServer(client.configs().back()).host_port_pair().port(), 9);

  // Later profile updates do not reopen it.
  const size_t count = client.configs().size();
  profile_monitor_->OnProxyConfigUpdated(
      net::ProxyConfigWithAnnotation::CreateDirect());
  profile_monitor_.FlushForTesting();
  client.Flush();
  EXPECT_EQ(client.configs().size(), count);
}

TEST_F(KoContainerProxyServiceTest, PendingWithoutEncryptor) {
  ASSERT_TRUE(SetProxy("socks5h://u:p@100.64.0.10:1080"));
  service_.reset();
  service_ = CreateService(/*os_crypt=*/nullptr);
  EXPECT_FALSE(service_->encryptor_ready_for_testing());

  // Requests wait for a config rather than going out without the proxy.
  auto params = ConfigureContainerContext();
  EXPECT_FALSE(params->initial_proxy_config);
  EXPECT_TRUE(params->proxy_config_client_receiver.is_valid());

  // New credentials are not stored in plain text meanwhile.
  bool called = false;
  auto parsed = ParseContainerProxyUrl("socks5h://u:q@100.64.0.10:1080");
  service_->SetProxy(
      kContainerId, std::move(*parsed), false,
      base::BindOnce([](bool* called, bool) { *called = true; }, &called));
  EXPECT_FALSE(called);
}

TEST_F(KoContainerProxyServiceTest, PruneRemovesUnknownContainers) {
  ASSERT_TRUE(SetProxy("socks5h://100.64.0.10:1080"));
  container_exists_ = true;
  service_->PruneStaleEntries();
  EXPECT_TRUE(service_->HasProxy(kContainerId));
  container_exists_ = false;
  service_->PruneStaleEntries();
  EXPECT_FALSE(service_->HasProxy(kContainerId));
}

}  // namespace containers
