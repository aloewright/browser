// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef BRAVE_COMPONENTS_KO_MACHINES_CORE_ADDRESS_ALLOWLIST_H_
#define BRAVE_COMPONENTS_KO_MACHINES_CORE_ADDRESS_ALLOWLIST_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "base/values.h"
#include "net/base/address_list.h"
#include "net/base/ip_address.h"

namespace ko_machines {

// Destinations the machine bridge may connect to. Only blocks that lie fully
// inside private, CGNAT (NetBird) or loopback space are accepted, so a
// misconfigured list still never sends plaintext RFB to a public address.
class AddressAllowlist {
 public:
  struct Block {
    net::IPAddress prefix;
    size_t prefix_length = 0;
  };

  AddressAllowlist();
  AddressAllowlist(const AddressAllowlist&);
  AddressAllowlist& operator=(const AddressAllowlist&);
  AddressAllowlist(AddressAllowlist&&);
  AddressAllowlist& operator=(AddressAllowlist&&);
  ~AddressAllowlist();

  // Builds a list from CIDR strings. Entries that are malformed or not fully
  // inside non-public space are skipped and, if `rejected` is set, appended to
  // it.
  static AddressAllowlist FromCidrs(
      const std::vector<std::string>& cidrs,
      std::vector<std::string>* rejected = nullptr);
  // Same, from a list of string values. Non-string values are ignored.
  static AddressAllowlist FromList(
      const base::ListValue& cidrs,
      std::vector<std::string>* rejected = nullptr);

  // True if `address` is inside an allowed block. IPv4-mapped IPv6 addresses
  // are matched as IPv4.
  bool Contains(const net::IPAddress& address) const;

  // The allowed endpoints of `addresses`, normalized to IPv4 where mapped and
  // with `port` applied.
  net::AddressList Filter(const net::AddressList& addresses,
                          uint16_t port) const;

  bool empty() const { return blocks_.empty(); }
  const std::vector<Block>& blocks() const { return blocks_; }

 private:
  std::vector<Block> blocks_;
};

// Parses `cidr` into `block`. Returns false if it is malformed or reaches
// outside non-public space.
bool ParseAllowedBlock(std::string_view cidr, AddressAllowlist::Block* block);

}  // namespace ko_machines

#endif  // BRAVE_COMPONENTS_KO_MACHINES_CORE_ADDRESS_ALLOWLIST_H_
