// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/ko/machines/core/address_allowlist.h"

#include <array>
#include <utility>

#include "net/base/ip_endpoint.h"

namespace ko_machines {

namespace {

// Address space that is never routed on the public internet. An allowlist
// block must sit entirely inside one of these.
constexpr auto kNonPublicRanges = std::to_array<std::string_view>({
    "10.0.0.0/8",
    "100.64.0.0/10",  // CGNAT, used by NetBird and Tailscale.
    "127.0.0.0/8",
    "172.16.0.0/12",
    "192.168.0.0/16",
    "::1/128",
    "fc00::/7",
});

net::IPAddress Normalize(const net::IPAddress& address) {
  return address.IsIPv4MappedIPv6() ? net::ConvertIPv4MappedIPv6ToIPv4(address)
                                    : address;
}

bool ParseBlock(std::string_view cidr, AddressAllowlist::Block* block) {
  net::IPAddress prefix;
  size_t prefix_length = 0;
  if (!net::ParseCIDRBlock(cidr, &prefix, &prefix_length) ||
      !prefix.IsValid()) {
    return false;
  }
  if (prefix.IsIPv4MappedIPv6()) {
    if (prefix_length < 96) {
      return false;
    }
    prefix = net::ConvertIPv4MappedIPv6ToIPv4(prefix);
    prefix_length -= 96;
  }
  block->prefix = std::move(prefix);
  block->prefix_length = prefix_length;
  return true;
}

bool IsInside(const AddressAllowlist::Block& inner,
              const AddressAllowlist::Block& outer) {
  return inner.prefix.size() == outer.prefix.size() &&
         inner.prefix_length >= outer.prefix_length &&
         net::IPAddressMatchesPrefix(inner.prefix, outer.prefix,
                                     outer.prefix_length);
}

bool IsNonPublic(const AddressAllowlist::Block& block) {
  for (std::string_view range : kNonPublicRanges) {
    AddressAllowlist::Block outer;
    if (ParseBlock(range, &outer) && IsInside(block, outer)) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool ParseAllowedBlock(std::string_view cidr, AddressAllowlist::Block* block) {
  AddressAllowlist::Block parsed;
  if (!ParseBlock(cidr, &parsed) || !IsNonPublic(parsed)) {
    return false;
  }
  *block = std::move(parsed);
  return true;
}

AddressAllowlist::AddressAllowlist() = default;
AddressAllowlist::AddressAllowlist(const AddressAllowlist&) = default;
AddressAllowlist& AddressAllowlist::operator=(const AddressAllowlist&) =
    default;
AddressAllowlist::AddressAllowlist(AddressAllowlist&&) = default;
AddressAllowlist& AddressAllowlist::operator=(AddressAllowlist&&) = default;
AddressAllowlist::~AddressAllowlist() = default;

// static
AddressAllowlist AddressAllowlist::FromCidrs(
    const std::vector<std::string>& cidrs,
    std::vector<std::string>* rejected) {
  AddressAllowlist allowlist;
  for (const std::string& cidr : cidrs) {
    Block block;
    if (ParseAllowedBlock(cidr, &block)) {
      allowlist.blocks_.push_back(std::move(block));
    } else if (rejected) {
      rejected->push_back(cidr);
    }
  }
  return allowlist;
}

// static
AddressAllowlist AddressAllowlist::FromList(
    const base::ListValue& cidrs,
    std::vector<std::string>* rejected) {
  std::vector<std::string> strings;
  for (const base::Value& value : cidrs) {
    if (const std::string* cidr = value.GetIfString()) {
      strings.push_back(*cidr);
    }
  }
  return FromCidrs(strings, rejected);
}

bool AddressAllowlist::Contains(const net::IPAddress& address) const {
  if (!address.IsValid()) {
    return false;
  }
  const net::IPAddress normalized = Normalize(address);
  for (const Block& block : blocks_) {
    if (block.prefix.size() == normalized.size() &&
        net::IPAddressMatchesPrefix(normalized, block.prefix,
                                    block.prefix_length)) {
      return true;
    }
  }
  return false;
}

net::AddressList AddressAllowlist::Filter(const net::AddressList& addresses,
                                          uint16_t port) const {
  net::AddressList allowed;
  for (const net::IPEndPoint& endpoint : addresses) {
    if (Contains(endpoint.address())) {
      allowed.push_back(net::IPEndPoint(Normalize(endpoint.address()), port));
    }
  }
  return allowed;
}

}  // namespace ko_machines
