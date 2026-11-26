// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/ko/machines/browser/machine_address_resolver.h"

#include <optional>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "brave/components/ko/machines/core/machine.h"
#include "net/base/host_port_pair.h"
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/base/network_anonymization_key.h"
#include "net/dns/public/host_resolver_source.h"
#include "services/network/public/cpp/simple_host_resolver.h"
#include "services/network/public/mojom/host_resolver.mojom.h"

namespace ko_machines {

namespace {

void PostResult(MachineAddressResolver::ResolveCallback callback,
                base::expected<net::AddressList, ResolveError> result) {
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), std::move(result)));
}

}  // namespace

MachineAddressResolver::MachineAddressResolver(
    NetworkContextGetter network_context_getter)
    : resolver_(network::SimpleHostResolver::Create(
          std::move(network_context_getter))) {}

MachineAddressResolver::~MachineAddressResolver() = default;

void MachineAddressResolver::Resolve(const std::string& host,
                                     uint16_t port,
                                     AddressAllowlist allowlist,
                                     ResolveCallback callback) {
  if (allowlist.empty()) {
    PostResult(std::move(callback),
               base::unexpected(ResolveError::kNoAllowlist));
    return;
  }
  const std::optional<std::string> canonical = CanonicalizeMachineHost(host);
  if (!canonical || port == 0) {
    PostResult(std::move(callback),
               base::unexpected(ResolveError::kInvalidHost));
    return;
  }

  net::IPAddress literal;
  if (net::ParseURLHostnameToAddress(*canonical, &literal)) {
    net::AddressList allowed = allowlist.Filter(
        net::AddressList(net::IPEndPoint(literal, port)), port);
    if (allowed.empty()) {
      PostResult(std::move(callback),
                 base::unexpected(ResolveError::kNotAllowed));
    } else {
      PostResult(std::move(callback), std::move(allowed));
    }
    return;
  }

  auto parameters = network::mojom::ResolveHostParameters::New();
  // getaddrinfo honours the per-domain resolvers NetBird installs.
  parameters->source = net::HostResolverSource::SYSTEM;
  // Never send machine names to a DoH provider.
  parameters->secure_dns_policy = network::mojom::SecureDnsPolicy::DISABLE;

  resolver_->ResolveHost(
      network::mojom::HostResolverHost::NewHostPortPair(
          net::HostPortPair(*canonical, port)),
      net::NetworkAnonymizationKey::CreateTransient(), std::move(parameters),
      base::BindOnce(&MachineAddressResolver::OnResolved,
                     weak_factory_.GetWeakPtr(), port, std::move(allowlist),
                     std::move(callback)));
}

void MachineAddressResolver::OnResolved(
    uint16_t port,
    AddressAllowlist allowlist,
    ResolveCallback callback,
    int result,
    const net::ResolveErrorInfo& resolve_error_info,
    const net::AddressList& resolved_addresses,
    const net::HostResolverEndpointResults& endpoints) {
  if (result != net::OK) {
    std::move(callback).Run(base::unexpected(ResolveError::kResolveFailed));
    return;
  }
  net::AddressList allowed = allowlist.Filter(resolved_addresses, port);
  if (allowed.empty()) {
    std::move(callback).Run(base::unexpected(ResolveError::kNotAllowed));
    return;
  }
  std::move(callback).Run(std::move(allowed));
}

}  // namespace ko_machines
