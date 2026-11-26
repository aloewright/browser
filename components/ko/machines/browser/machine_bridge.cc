// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/ko/machines/browser/machine_bridge.h"

#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "brave/components/ko/machines/core/machine.h"
#include "brave/components/ko/machines/core/prefs.h"
#include "net/base/net_errors.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/mojom/network_context.mojom.h"

namespace ko_machines {

namespace {

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("ko_machine_bridge", R"(
        semantics {
          sender: "K-O Machines"
          description:
            "Remote desktop (RFB/VNC) connection to a machine the user added "
            "in chrome://ko-machines."
          trigger: "The user opens the machine's desktop."
          data:
            "RFB traffic: screen updates, keyboard and pointer input, and "
            "clipboard text if the user turns clipboard sharing on."
          destination: OTHER
          destination_other:
            "A host the user configured, connected only at addresses inside "
            "the private address allowlist (default 100.64.0.0/10, the "
            "NetBird range)."
        }
        policy {
          cookies_allowed: NO
          setting: "Remove the machine in chrome://ko-machines."
          policy_exception_justification: "Personal build without policies."
        }
      )");

// TCP keepalive so a dead peer is noticed.
constexpr uint16_t kKeepAliveDelaySeconds = 60;

mojom::ConnectResult ToConnectResult(ResolveError error) {
  switch (error) {
    case ResolveError::kInvalidHost:
    case ResolveError::kResolveFailed:
      return mojom::ConnectResult::kResolveFailed;
    case ResolveError::kNoAllowlist:
      return mojom::ConnectResult::kNoAllowlist;
    case ResolveError::kNotAllowed:
      return mojom::ConnectResult::kNotAllowed;
  }
}

void Fail(mojom::KOMachineBridge::ConnectCallback callback,
          mojom::ConnectResult result) {
  std::move(callback).Run(result, mojo::ScopedDataPipeConsumerHandle(),
                          mojo::ScopedDataPipeProducerHandle());
}

// Owns an open socket. Destroyed, and the socket closed, when the page drops
// its MachineConnection remote.
class OpenConnection : public mojom::MachineConnection {
 public:
  explicit OpenConnection(
      mojo::Remote<network::mojom::TCPConnectedSocket> socket)
      : socket_(std::move(socket)) {}
  OpenConnection(const OpenConnection&) = delete;
  OpenConnection& operator=(const OpenConnection&) = delete;
  ~OpenConnection() override = default;

 private:
  mojo::Remote<network::mojom::TCPConnectedSocket> socket_;
};

}  // namespace

MachineBridge::MachineBridge(
    mojo::PendingReceiver<mojom::KOMachineBridge> receiver,
    PrefService& prefs,
    NetworkContextGetter network_context_getter)
    : prefs_(prefs),
      network_context_getter_(network_context_getter),
      resolver_(std::move(network_context_getter)),
      receiver_(this, std::move(receiver)) {}

MachineBridge::~MachineBridge() = default;

void MachineBridge::Connect(
    const std::string& machine_id,
    mojo::PendingReceiver<mojom::MachineConnection> connection,
    ConnectCallback callback) {
  if (connections_.size() + pending_connects_ >= kMaxConnectionsPerPage) {
    Fail(std::move(callback), mojom::ConnectResult::kTooManyConnections);
    return;
  }
  // Only machines stored in prefs can be reached; the page never supplies an
  // address.
  std::optional<Machine> machine = FindMachine(*prefs_, machine_id);
  if (!machine) {
    Fail(std::move(callback), mojom::ConnectResult::kUnknownMachine);
    return;
  }
  AddressAllowlist allowlist = GetAllowlist(*prefs_);
  if (allowlist.empty()) {
    Fail(std::move(callback), mojom::ConnectResult::kNoAllowlist);
    return;
  }

  ++pending_connects_;
  AddressAllowlist resolve_allowlist = allowlist;
  resolver_.Resolve(
      machine->host, machine->port, std::move(resolve_allowlist),
      base::BindOnce(&MachineBridge::OnResolved, weak_factory_.GetWeakPtr(),
                     std::move(allowlist), std::move(connection),
                     std::move(callback)));
}

void MachineBridge::OnResolved(
    AddressAllowlist allowlist,
    mojo::PendingReceiver<mojom::MachineConnection> connection,
    ConnectCallback callback,
    base::expected<net::AddressList, ResolveError> addresses) {
  if (!addresses.has_value()) {
    --pending_connects_;
    Fail(std::move(callback), ToConnectResult(addresses.error()));
    return;
  }
  network::mojom::NetworkContext* network_context =
      network_context_getter_.Run();
  if (!network_context) {
    --pending_connects_;
    Fail(std::move(callback), mojom::ConnectResult::kConnectFailed);
    return;
  }

  auto options = network::mojom::TCPConnectedSocketOptions::New();
  options->no_delay = true;
  options->keep_alive_options = network::mojom::TCPKeepAliveOptions::New(
      /*enable=*/true, kKeepAliveDelaySeconds);

  mojo::Remote<network::mojom::TCPConnectedSocket> socket;
  auto socket_receiver = socket.BindNewPipeAndPassReceiver();
  // Connects to the vetted addresses only; nothing is re-resolved.
  network_context->CreateTCPConnectedSocket(
      /*local_addr=*/std::nullopt, *addresses, std::move(options),
      net::MutableNetworkTrafficAnnotationTag(kTrafficAnnotation),
      std::move(socket_receiver), /*observer=*/mojo::NullRemote(),
      base::BindOnce(&MachineBridge::OnConnected, weak_factory_.GetWeakPtr(),
                     std::move(allowlist), std::move(socket),
                     std::move(connection), std::move(callback)));
}

void MachineBridge::OnConnected(
    AddressAllowlist allowlist,
    mojo::Remote<network::mojom::TCPConnectedSocket> socket,
    mojo::PendingReceiver<mojom::MachineConnection> connection,
    ConnectCallback callback,
    int32_t result,
    const std::optional<net::IPEndPoint>& local_addr,
    const std::optional<net::IPEndPoint>& peer_addr,
    mojo::ScopedDataPipeConsumerHandle receive_stream,
    mojo::ScopedDataPipeProducerHandle send_stream) {
  --pending_connects_;
  // Fail closed: the peer must still be an allowlisted address.
  if (result != net::OK || !peer_addr ||
      !allowlist.Contains(peer_addr->address()) || !receive_stream.is_valid() ||
      !send_stream.is_valid()) {
    Fail(std::move(callback), mojom::ConnectResult::kConnectFailed);
    return;
  }
  connections_.Add(std::make_unique<OpenConnection>(std::move(socket)),
                   std::move(connection));
  std::move(callback).Run(mojom::ConnectResult::kOk, std::move(receive_stream),
                          std::move(send_stream));
}

}  // namespace ko_machines
