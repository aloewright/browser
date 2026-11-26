// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/ko/machines/browser/machine_bridge.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/test/run_until.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "brave/components/ko/machines/core/machine.h"
#include "brave/components/ko/machines/core/prefs.h"
#include "components/prefs/testing_pref_service.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/message_pipe.h"
#include "net/base/ip_address.h"
#include "net/base/net_errors.h"
#include "net/dns/mock_host_resolver.h"
#include "services/network/test/test_network_context_with_host_resolver.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ko_machines {

namespace {

net::IPEndPoint Endpoint(std::string_view literal, uint16_t port) {
  std::optional<net::IPAddress> address =
      net::IPAddress::FromIPLiteral(literal);
  CHECK(address);
  return net::IPEndPoint(*address, port);
}

// Records every connect request and answers it with fresh data pipes.
class FakeNetworkContext : public network::TestNetworkContextWithHostResolver {
 public:
  FakeNetworkContext()
      : TestNetworkContextWithHostResolver(
            std::make_unique<net::MockHostResolver>()) {}

  void CreateTCPConnectedSocket(
      const std::optional<net::IPEndPoint>& local_addr,
      const net::AddressList& remote_addr_list,
      network::mojom::TCPConnectedSocketOptionsPtr tcp_connected_socket_options,
      const net::MutableNetworkTrafficAnnotationTag& traffic_annotation,
      mojo::PendingReceiver<network::mojom::TCPConnectedSocket> socket,
      mojo::PendingRemote<network::mojom::SocketObserver> observer,
      CreateTCPConnectedSocketCallback callback) override {
    requests_.push_back(remote_addr_list);
    if (connect_error_ != net::OK) {
      std::move(callback).Run(connect_error_, std::nullopt, std::nullopt,
                              mojo::ScopedDataPipeConsumerHandle(),
                              mojo::ScopedDataPipeProducerHandle());
      return;
    }
    mojo::ScopedDataPipeProducerHandle receive_producer;
    mojo::ScopedDataPipeConsumerHandle receive_consumer;
    mojo::ScopedDataPipeProducerHandle send_producer;
    mojo::ScopedDataPipeConsumerHandle send_consumer;
    CHECK_EQ(MOJO_RESULT_OK,
             mojo::CreateDataPipe(1024, receive_producer, receive_consumer));
    CHECK_EQ(MOJO_RESULT_OK,
             mojo::CreateDataPipe(1024, send_producer, send_consumer));
    socket_pipes_.push_back(socket.PassPipe());
    const net::IPEndPoint peer =
        peer_override_.value_or(remote_addr_list.endpoints().front());
    std::move(callback).Run(net::OK, Endpoint("100.64.0.1", 50000), peer,
                            std::move(receive_consumer),
                            std::move(send_producer));
  }

  net::MockHostResolver::RuleResolver& rules() {
    return *static_cast<net::MockHostResolver*>(host_resolver())->rules();
  }

  const std::vector<net::AddressList>& requests() const { return requests_; }
  // True once the browser dropped its end of the `index`th socket.
  bool IsSocketClosed(size_t index) const {
    return socket_pipes_[index]->QuerySignalsState().peer_closed();
  }
  void set_connect_error(int error) { connect_error_ = error; }
  void set_peer_override(net::IPEndPoint peer) { peer_override_ = peer; }

 private:
  std::vector<net::AddressList> requests_;
  std::vector<mojo::ScopedMessagePipeHandle> socket_pipes_;
  int connect_error_ = net::OK;
  std::optional<net::IPEndPoint> peer_override_;
};

struct ConnectResponse {
  mojom::ConnectResult result;
  bool has_streams;
};

}  // namespace

class KOMachineBridgeTest : public testing::Test {
 protected:
  KOMachineBridgeTest() {
    RegisterProfilePrefs(prefs_.registry());
    bridge_ = std::make_unique<MachineBridge>(
        remote_.BindNewPipeAndPassReceiver(), prefs_,
        base::BindRepeating(&KOMachineBridgeTest::GetNetworkContext,
                            base::Unretained(this)));
  }

  void AddMachine(const std::string& id, const std::string& host) {
    Machine machine;
    machine.id = id;
    machine.name = id;
    machine.host = host;
    std::vector<Machine> machines = GetMachines(prefs_);
    machines.push_back(machine);
    SetMachines(prefs_, machines);
  }

  ConnectResponse Connect(
      const std::string& id,
      mojo::Remote<mojom::MachineConnection>* connection = nullptr) {
    mojo::Remote<mojom::MachineConnection> local_connection;
    mojo::Remote<mojom::MachineConnection>& target =
        connection ? *connection : local_connection;
    base::test::TestFuture<mojom::ConnectResult,
                           mojo::ScopedDataPipeConsumerHandle,
                           mojo::ScopedDataPipeProducerHandle>
        future;
    remote_->Connect(id, target.BindNewPipeAndPassReceiver(),
                     future.GetCallback());
    auto [result, receive, send] = future.Take();
    return {result, receive.is_valid() && send.is_valid()};
  }

  network::mojom::NetworkContext* GetNetworkContext() {
    return &network_context_;
  }

  base::test::TaskEnvironment task_environment_;
  TestingPrefServiceSimple prefs_;
  FakeNetworkContext network_context_;
  mojo::Remote<mojom::KOMachineBridge> remote_;
  std::unique_ptr<MachineBridge> bridge_;
};

TEST_F(KOMachineBridgeTest, ConnectsToAllowlistedMachine) {
  AddMachine("studio", "100.64.0.10");
  ConnectResponse response = Connect("studio");
  EXPECT_EQ(mojom::ConnectResult::kOk, response.result);
  EXPECT_TRUE(response.has_streams);
  ASSERT_EQ(1u, network_context_.requests().size());
  ASSERT_EQ(1u, network_context_.requests()[0].size());
  EXPECT_EQ(Endpoint("100.64.0.10", 5900),
            network_context_.requests()[0].endpoints()[0]);
}

TEST_F(KOMachineBridgeTest, UnknownMachineNeverConnects) {
  EXPECT_EQ(mojom::ConnectResult::kUnknownMachine,
            Connect("100.64.0.10").result);
  EXPECT_TRUE(network_context_.requests().empty());
}

TEST_F(KOMachineBridgeTest, PublicDestinationNeverConnects) {
  AddMachine("public", "93.184.216.34");
  network_context_.rules().AddRule("public.example", "93.184.216.34");
  AddMachine("public-name", "public.example");

  EXPECT_EQ(mojom::ConnectResult::kNotAllowed, Connect("public").result);
  EXPECT_EQ(mojom::ConnectResult::kNotAllowed, Connect("public-name").result);
  EXPECT_TRUE(network_context_.requests().empty());
}

TEST_F(KOMachineBridgeTest, OnlyAllowlistedAddressesAreDialed) {
  network_context_.rules().AddRule("mixed.example",
                                   "93.184.216.34,100.64.0.30");
  AddMachine("mixed", "mixed.example");
  EXPECT_EQ(mojom::ConnectResult::kOk, Connect("mixed").result);
  ASSERT_EQ(1u, network_context_.requests().size());
  ASSERT_EQ(1u, network_context_.requests()[0].size());
  EXPECT_EQ(Endpoint("100.64.0.30", 5900),
            network_context_.requests()[0].endpoints()[0]);
}

TEST_F(KOMachineBridgeTest, EmptyAllowlistFailsClosed) {
  AddMachine("studio", "100.64.0.10");
  SetAllowedCidrs(prefs_, {});
  EXPECT_EQ(mojom::ConnectResult::kNoAllowlist, Connect("studio").result);
  EXPECT_TRUE(network_context_.requests().empty());
}

TEST_F(KOMachineBridgeTest, UnexpectedPeerIsRejected) {
  AddMachine("studio", "100.64.0.10");
  network_context_.set_peer_override(Endpoint("93.184.216.34", 5900));
  ConnectResponse response = Connect("studio");
  EXPECT_EQ(mojom::ConnectResult::kConnectFailed, response.result);
  EXPECT_FALSE(response.has_streams);
}

TEST_F(KOMachineBridgeTest, ConnectErrorIsReported) {
  AddMachine("studio", "100.64.0.10");
  network_context_.set_connect_error(net::ERR_CONNECTION_REFUSED);
  ConnectResponse response = Connect("studio");
  EXPECT_EQ(mojom::ConnectResult::kConnectFailed, response.result);
  EXPECT_FALSE(response.has_streams);
}

TEST_F(KOMachineBridgeTest, LimitsOpenConnections) {
  AddMachine("studio", "100.64.0.10");
  std::vector<mojo::Remote<mojom::MachineConnection>> connections(
      kMaxConnectionsPerPage);
  for (auto& connection : connections) {
    EXPECT_EQ(mojom::ConnectResult::kOk, Connect("studio", &connection).result);
  }
  EXPECT_EQ(mojom::ConnectResult::kTooManyConnections,
            Connect("studio").result);
  EXPECT_EQ(kMaxConnectionsPerPage, network_context_.requests().size());

  // Dropping a connection closes its socket and frees the slot.
  connections.pop_back();
  ASSERT_TRUE(base::test::RunUntil([&] {
    return network_context_.IsSocketClosed(kMaxConnectionsPerPage - 1);
  }));
  EXPECT_FALSE(network_context_.IsSocketClosed(0));
  EXPECT_EQ(mojom::ConnectResult::kOk, Connect("studio").result);
}

TEST_F(KOMachineBridgeTest, DestroyingBridgeClosesSockets) {
  AddMachine("studio", "100.64.0.10");
  mojo::Remote<mojom::MachineConnection> connection;
  ASSERT_EQ(mojom::ConnectResult::kOk, Connect("studio", &connection).result);
  EXPECT_FALSE(network_context_.IsSocketClosed(0));
  bridge_.reset();
  EXPECT_TRUE(
      base::test::RunUntil([&] { return network_context_.IsSocketClosed(0); }));
}

}  // namespace ko_machines
