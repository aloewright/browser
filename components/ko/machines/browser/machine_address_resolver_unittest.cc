// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/ko/machines/browser/machine_address_resolver.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "net/dns/mock_host_resolver.h"
#include "services/network/test/test_network_context_with_host_resolver.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ko_machines {

namespace {

using Result = base::expected<net::AddressList, ResolveError>;

net::IPEndPoint Endpoint(std::string_view literal, uint16_t port) {
  std::optional<net::IPAddress> address =
      net::IPAddress::FromIPLiteral(literal);
  CHECK(address);
  return net::IPEndPoint(*address, port);
}

}  // namespace

class KOMachineAddressResolverTest : public testing::Test {
 protected:
  KOMachineAddressResolverTest()
      : network_context_(std::make_unique<net::MockHostResolver>()),
        resolver_(base::BindRepeating(
            &KOMachineAddressResolverTest::GetNetworkContext,
            base::Unretained(this))) {}

  net::MockHostResolver::RuleResolver& rules() {
    return *static_cast<net::MockHostResolver*>(
                network_context_.host_resolver())
                ->rules();
  }

  Result Resolve(const std::string& host,
                 uint16_t port = 5900,
                 const std::vector<std::string>& cidrs = {"100.64.0.0/10"}) {
    base::test::TestFuture<Result> future;
    resolver_.Resolve(host, port, AddressAllowlist::FromCidrs(cidrs),
                      future.GetCallback());
    return future.Take();
  }

 private:
  network::mojom::NetworkContext* GetNetworkContext() {
    return &network_context_;
  }

  base::test::TaskEnvironment task_environment_;
  network::TestNetworkContextWithHostResolver network_context_;
  MachineAddressResolver resolver_;
};

TEST_F(KOMachineAddressResolverTest, AllowedIPLiteral) {
  Result result = Resolve("100.64.0.10");
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(1u, result->size());
  EXPECT_EQ(Endpoint("100.64.0.10", 5900), result->endpoints()[0]);
}

TEST_F(KOMachineAddressResolverTest, PublicIPLiteralIsRefused) {
  Result result = Resolve("93.184.216.34");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(ResolveError::kNotAllowed, result.error());
}

TEST_F(KOMachineAddressResolverTest, LanLiteralNeedsItsOwnBlock) {
  EXPECT_EQ(ResolveError::kNotAllowed, Resolve("192.168.1.20").error());
  Result result = Resolve("192.168.1.20", 5901, {"192.168.1.0/24"});
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(Endpoint("192.168.1.20", 5901), result->endpoints()[0]);
}

TEST_F(KOMachineAddressResolverTest, IPv6Literal) {
  Result result = Resolve("[fd00::5]", 5900, {"fd00::/8"});
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(Endpoint("fd00::5", 5900), result->endpoints()[0]);
  EXPECT_EQ(ResolveError::kNotAllowed,
            Resolve("[2001:db8::5]", 5900, {"fd00::/8"}).error());
}

TEST_F(KOMachineAddressResolverTest, HostnameResolvingToAllowedAddress) {
  rules().AddRule("mac.netbird.example", "100.64.0.20");
  Result result = Resolve("mac.netbird.example", 5901);
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(1u, result->size());
  EXPECT_EQ(Endpoint("100.64.0.20", 5901), result->endpoints()[0]);
}

TEST_F(KOMachineAddressResolverTest, HostnameResolvingToPublicAddress) {
  rules().AddRule("public.example", "93.184.216.34");
  EXPECT_EQ(ResolveError::kNotAllowed, Resolve("public.example").error());
}

TEST_F(KOMachineAddressResolverTest, DnsRebindToLoopbackIsRefused) {
  rules().AddRule("rebind.example", "127.0.0.1");
  EXPECT_EQ(ResolveError::kNotAllowed, Resolve("rebind.example").error());
}

TEST_F(KOMachineAddressResolverTest, MixedAnswerKeepsOnlyAllowedAddresses) {
  rules().AddRule("mixed.example", "93.184.216.34,100.64.0.30,10.0.0.5");
  Result result = Resolve("mixed.example");
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(1u, result->size());
  EXPECT_EQ(Endpoint("100.64.0.30", 5900), result->endpoints()[0]);
}

TEST_F(KOMachineAddressResolverTest, ResolutionFailure) {
  rules().AddSimulatedFailure("missing.example");
  EXPECT_EQ(ResolveError::kResolveFailed, Resolve("missing.example").error());
}

TEST_F(KOMachineAddressResolverTest, EmptyAllowlistRefusesEverything) {
  EXPECT_EQ(ResolveError::kNoAllowlist,
            Resolve("100.64.0.10", 5900, {}).error());
  EXPECT_EQ(ResolveError::kNoAllowlist,
            Resolve("100.64.0.10", 5900, {"0.0.0.0/0"}).error());
}

TEST_F(KOMachineAddressResolverTest, InvalidInput) {
  EXPECT_EQ(ResolveError::kInvalidHost, Resolve("two words").error());
  EXPECT_EQ(ResolveError::kInvalidHost, Resolve("host.example:22").error());
  EXPECT_EQ(ResolveError::kInvalidHost, Resolve("100.64.0.10", 0).error());
}

}  // namespace ko_machines
