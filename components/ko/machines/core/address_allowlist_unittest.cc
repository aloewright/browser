// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/ko/machines/core/address_allowlist.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/check.h"
#include "base/values.h"
#include "net/base/address_list.h"
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ko_machines {

namespace {

net::IPAddress Ip(std::string_view literal) {
  std::optional<net::IPAddress> address =
      net::IPAddress::FromIPLiteral(literal);
  CHECK(address);
  return *address;
}

}  // namespace

TEST(KOAddressAllowlistTest, DefaultNetBirdRange) {
  const AddressAllowlist allowlist =
      AddressAllowlist::FromCidrs({"100.64.0.0/10"});
  ASSERT_EQ(1u, allowlist.blocks().size());

  EXPECT_TRUE(allowlist.Contains(Ip("100.64.0.1")));
  EXPECT_TRUE(allowlist.Contains(Ip("100.64.0.10")));
  EXPECT_TRUE(allowlist.Contains(Ip("100.127.255.254")));

  EXPECT_FALSE(allowlist.Contains(Ip("100.63.255.255")));
  EXPECT_FALSE(allowlist.Contains(Ip("100.128.0.1")));
  EXPECT_FALSE(allowlist.Contains(Ip("192.168.1.10")));
  EXPECT_FALSE(allowlist.Contains(Ip("8.8.8.8")));
  EXPECT_FALSE(allowlist.Contains(Ip("fd00::1")));
}

TEST(KOAddressAllowlistTest, RejectsPublicAndMalformedBlocks) {
  std::vector<std::string> rejected;
  const AddressAllowlist allowlist = AddressAllowlist::FromCidrs(
      {"0.0.0.0/0", "8.8.8.0/24", "100.0.0.0/8", "172.0.0.0/8", "::/0",
       "2001:db8::/32", "::ffff:0:0/96", "100.64.0.0/33", "100.64.0.0",
       "not-a-cidr", ""},
      &rejected);
  EXPECT_TRUE(allowlist.empty());
  EXPECT_EQ(11u, rejected.size());

  EXPECT_FALSE(allowlist.Contains(Ip("100.64.0.10")));
  EXPECT_FALSE(allowlist.Contains(Ip("8.8.8.8")));
}

TEST(KOAddressAllowlistTest, AcceptsBlocksInsidePrivateSpace) {
  std::vector<std::string> rejected;
  const AddressAllowlist allowlist = AddressAllowlist::FromCidrs(
      {"10.1.0.0/16", "172.16.5.0/24", "192.168.1.0/24", "127.0.0.1/32",
       "100.64.0.10/32", "fd00::/8", "::1/128"},
      &rejected);
  EXPECT_TRUE(rejected.empty());
  EXPECT_EQ(7u, allowlist.blocks().size());

  EXPECT_TRUE(allowlist.Contains(Ip("10.1.2.3")));
  EXPECT_FALSE(allowlist.Contains(Ip("10.2.0.1")));
  EXPECT_TRUE(allowlist.Contains(Ip("172.16.5.200")));
  EXPECT_TRUE(allowlist.Contains(Ip("192.168.1.1")));
  EXPECT_TRUE(allowlist.Contains(Ip("127.0.0.1")));
  EXPECT_FALSE(allowlist.Contains(Ip("127.0.0.2")));
  EXPECT_TRUE(allowlist.Contains(Ip("100.64.0.10")));
  EXPECT_FALSE(allowlist.Contains(Ip("100.64.0.11")));
  EXPECT_TRUE(allowlist.Contains(Ip("fd12:3456::1")));
  EXPECT_FALSE(allowlist.Contains(Ip("fe80::1")));
  EXPECT_TRUE(allowlist.Contains(Ip("::1")));
}

TEST(KOAddressAllowlistTest, BlockMustFitInsideOnePrivateRange) {
  // Wider than 192.168.0.0/16, so it reaches public space.
  EXPECT_TRUE(AddressAllowlist::FromCidrs({"192.168.0.0/15"}).empty());
  // Wider than 172.16.0.0/12.
  EXPECT_TRUE(AddressAllowlist::FromCidrs({"172.16.0.0/11"}).empty());
  // Wider than 100.64.0.0/10.
  EXPECT_TRUE(AddressAllowlist::FromCidrs({"100.64.0.0/9"}).empty());
  // Wider than fc00::/7.
  EXPECT_TRUE(AddressAllowlist::FromCidrs({"fc00::/6"}).empty());
}

TEST(KOAddressAllowlistTest, MappedAddressesMatchAsIPv4) {
  const AddressAllowlist allowlist =
      AddressAllowlist::FromCidrs({"100.64.0.0/10"});
  EXPECT_TRUE(allowlist.Contains(Ip("::ffff:100.64.0.10")));
  EXPECT_FALSE(allowlist.Contains(Ip("::ffff:8.8.8.8")));
}

TEST(KOAddressAllowlistTest, MappedPrefixIsNormalized) {
  std::vector<std::string> rejected;
  const AddressAllowlist allowlist =
      AddressAllowlist::FromCidrs({"::ffff:100.64.0.0/106"}, &rejected);
  EXPECT_TRUE(rejected.empty());
  ASSERT_EQ(1u, allowlist.blocks().size());
  EXPECT_TRUE(allowlist.blocks()[0].prefix.IsIPv4());
  EXPECT_EQ(10u, allowlist.blocks()[0].prefix_length);
  EXPECT_TRUE(allowlist.Contains(Ip("100.64.0.10")));
}

TEST(KOAddressAllowlistTest, FamiliesDoNotCrossMatch) {
  const AddressAllowlist allowlist = AddressAllowlist::FromCidrs({"fd00::/8"});
  EXPECT_FALSE(allowlist.Contains(Ip("10.0.0.1")));
  EXPECT_FALSE(allowlist.Contains(Ip("100.64.0.1")));
}

TEST(KOAddressAllowlistTest, InvalidAddressIsNotContained) {
  const AddressAllowlist allowlist =
      AddressAllowlist::FromCidrs({"100.64.0.0/10"});
  EXPECT_FALSE(allowlist.Contains(net::IPAddress()));
}

TEST(KOAddressAllowlistTest, EmptyAllowlistContainsNothing) {
  const AddressAllowlist allowlist;
  EXPECT_TRUE(allowlist.empty());
  EXPECT_FALSE(allowlist.Contains(Ip("100.64.0.10")));
  EXPECT_FALSE(allowlist.Contains(Ip("127.0.0.1")));
}

TEST(KOAddressAllowlistTest, FilterKeepsAllowedEndpointsAndAppliesPort) {
  const AddressAllowlist allowlist =
      AddressAllowlist::FromCidrs({"100.64.0.0/10"});
  net::AddressList resolved;
  resolved.push_back(net::IPEndPoint(Ip("93.184.216.34"), 0));
  resolved.push_back(net::IPEndPoint(Ip("100.64.0.10"), 0));
  resolved.push_back(net::IPEndPoint(Ip("::ffff:100.64.0.11"), 0));
  resolved.push_back(net::IPEndPoint(Ip("2001:db8::1"), 0));

  const net::AddressList filtered = allowlist.Filter(resolved, 5900);
  ASSERT_EQ(2u, filtered.size());
  EXPECT_EQ(net::IPEndPoint(Ip("100.64.0.10"), 5900), filtered.endpoints()[0]);
  EXPECT_EQ(net::IPEndPoint(Ip("100.64.0.11"), 5900), filtered.endpoints()[1]);
}

TEST(KOAddressAllowlistTest, FilterOfPublicOnlyIsEmpty) {
  const AddressAllowlist allowlist =
      AddressAllowlist::FromCidrs({"100.64.0.0/10"});
  net::AddressList resolved;
  resolved.push_back(net::IPEndPoint(Ip("93.184.216.34"), 5900));
  EXPECT_TRUE(allowlist.Filter(resolved, 5900).empty());
}

TEST(KOAddressAllowlistTest, FromListIgnoresNonStrings) {
  base::ListValue list;
  list.Append("100.64.0.0/10");
  list.Append(5);
  list.Append(base::DictValue());
  std::vector<std::string> rejected;
  const AddressAllowlist allowlist =
      AddressAllowlist::FromList(list, &rejected);
  EXPECT_EQ(1u, allowlist.blocks().size());
  EXPECT_TRUE(rejected.empty());
}

TEST(KOAddressAllowlistTest, ParseAllowedBlock) {
  AddressAllowlist::Block block;
  EXPECT_TRUE(ParseAllowedBlock("100.64.0.0/10", &block));
  EXPECT_EQ(Ip("100.64.0.0"), block.prefix);
  EXPECT_EQ(10u, block.prefix_length);
  EXPECT_FALSE(ParseAllowedBlock("100.64.0.0/8", &block));
  // A failed parse leaves the output untouched.
  EXPECT_EQ(10u, block.prefix_length);
}

}  // namespace ko_machines
