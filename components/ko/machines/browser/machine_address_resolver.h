// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef BRAVE_COMPONENTS_KO_MACHINES_BROWSER_MACHINE_ADDRESS_RESOLVER_H_
#define BRAVE_COMPONENTS_KO_MACHINES_BROWSER_MACHINE_ADDRESS_RESOLVER_H_

#include <cstdint>
#include <memory>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/types/expected.h"
#include "brave/components/ko/machines/core/address_allowlist.h"
#include "net/base/address_list.h"
#include "net/dns/public/host_resolver_results.h"
#include "net/dns/public/resolve_error_info.h"

namespace network {
class SimpleHostResolver;
namespace mojom {
class NetworkContext;
}  // namespace mojom
}  // namespace network

namespace ko_machines {

enum class ResolveError {
  kInvalidHost,
  kNoAllowlist,
  kResolveFailed,
  // Resolved, but to no allowlisted address.
  kNotAllowed,
};

// Turns a machine host into the endpoints the bridge may connect to. IP
// literals are checked directly; hostnames are resolved through the network
// service with the system resolver (so NetBird DNS works) and without DoH (so
// the name is not sent to a public resolver). Only allowlisted addresses are
// returned, and the caller connects to exactly those, so a later DNS answer
// cannot redirect the connection.
class MachineAddressResolver {
 public:
  using NetworkContextGetter =
      base::RepeatingCallback<network::mojom::NetworkContext*()>;
  using ResolveCallback =
      base::OnceCallback<void(base::expected<net::AddressList, ResolveError>)>;

  explicit MachineAddressResolver(NetworkContextGetter network_context_getter);
  MachineAddressResolver(const MachineAddressResolver&) = delete;
  MachineAddressResolver& operator=(const MachineAddressResolver&) = delete;
  ~MachineAddressResolver();

  // `host` is a canonical machine host (see CanonicalizeMachineHost()). The
  // callback is always run asynchronously.
  void Resolve(const std::string& host,
               uint16_t port,
               AddressAllowlist allowlist,
               ResolveCallback callback);

 private:
  void OnResolved(uint16_t port,
                  AddressAllowlist allowlist,
                  ResolveCallback callback,
                  int result,
                  const net::ResolveErrorInfo& resolve_error_info,
                  const net::AddressList& resolved_addresses,
                  const net::HostResolverEndpointResults& endpoints);

  std::unique_ptr<network::SimpleHostResolver> resolver_;
  base::WeakPtrFactory<MachineAddressResolver> weak_factory_{this};
};

}  // namespace ko_machines

#endif  // BRAVE_COMPONENTS_KO_MACHINES_BROWSER_MACHINE_ADDRESS_RESOLVER_H_
