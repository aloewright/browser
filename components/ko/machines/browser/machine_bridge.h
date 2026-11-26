// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef BRAVE_COMPONENTS_KO_MACHINES_BROWSER_MACHINE_BRIDGE_H_
#define BRAVE_COMPONENTS_KO_MACHINES_BROWSER_MACHINE_BRIDGE_H_

#include <cstddef>
#include <optional>
#include <string>

#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "brave/components/ko/machines/browser/machine_address_resolver.h"
#include "brave/components/ko/machines/common/ko_machines.mojom.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/unique_receiver_set.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "net/base/ip_endpoint.h"
#include "services/network/public/mojom/tcp_socket.mojom.h"

class PrefService;

namespace ko_machines {

// Most connections one chrome://ko-machines page may hold open at once.
inline constexpr size_t kMaxConnectionsPerPage = 4;

// mojom::KOMachineBridge. Opens raw TCP connections for noVNC through the
// network service, only to machines in prefs and only to allowlisted
// addresses. The socket's data pipes go straight to the page; the browser
// keeps the socket and closes it when the page drops its MachineConnection.
class MachineBridge : public mojom::KOMachineBridge {
 public:
  using NetworkContextGetter = MachineAddressResolver::NetworkContextGetter;

  MachineBridge(mojo::PendingReceiver<mojom::KOMachineBridge> receiver,
                PrefService& prefs,
                NetworkContextGetter network_context_getter);
  MachineBridge(const MachineBridge&) = delete;
  MachineBridge& operator=(const MachineBridge&) = delete;
  ~MachineBridge() override;

  // mojom::KOMachineBridge:
  void Connect(const std::string& machine_id,
               mojo::PendingReceiver<mojom::MachineConnection> connection,
               ConnectCallback callback) override;

 private:
  void OnResolved(AddressAllowlist allowlist,
                  mojo::PendingReceiver<mojom::MachineConnection> connection,
                  ConnectCallback callback,
                  base::expected<net::AddressList, ResolveError> addresses);
  void OnConnected(AddressAllowlist allowlist,
                   mojo::Remote<network::mojom::TCPConnectedSocket> socket,
                   mojo::PendingReceiver<mojom::MachineConnection> connection,
                   ConnectCallback callback,
                   int32_t result,
                   const std::optional<net::IPEndPoint>& local_addr,
                   const std::optional<net::IPEndPoint>& peer_addr,
                   mojo::ScopedDataPipeConsumerHandle receive_stream,
                   mojo::ScopedDataPipeProducerHandle send_stream);

  const raw_ref<PrefService> prefs_;
  NetworkContextGetter network_context_getter_;
  MachineAddressResolver resolver_;
  mojo::Receiver<mojom::KOMachineBridge> receiver_;
  // Each entry owns one open socket.
  mojo::UniqueReceiverSet<mojom::MachineConnection> connections_;
  size_t pending_connects_ = 0;
  base::WeakPtrFactory<MachineBridge> weak_factory_{this};
};

}  // namespace ko_machines

#endif  // BRAVE_COMPONENTS_KO_MACHINES_BROWSER_MACHINE_BRIDGE_H_
