// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/ko/machines/core/prefs.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "brave/components/ko/machines/core/pref_names.h"
#include "components/prefs/testing_pref_service.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ko_machines {

namespace {

Machine MakeMachine(std::string id, std::string host, uint16_t port) {
  Machine machine;
  machine.id = std::move(id);
  machine.name = "Machine " + machine.id;
  machine.host = std::move(host);
  machine.port = port;
  return machine;
}

}  // namespace

class KOMachinesPrefsTest : public testing::Test {
 protected:
  void SetUp() override { RegisterProfilePrefs(prefs_.registry()); }

  TestingPrefServiceSimple prefs_;
};

TEST_F(KOMachinesPrefsTest, Defaults) {
  EXPECT_TRUE(GetMachines(prefs_).empty());
  EXPECT_EQ(std::vector<std::string>{kDefaultAllowedCidr},
            GetAllowedCidrs(prefs_));
  EXPECT_FALSE(prefs_.GetBoolean(prefs::kSeeded));
  EXPECT_TRUE(prefs_.GetDict(prefs::kCredentials).empty());

  const AddressAllowlist allowlist = GetAllowlist(prefs_);
  ASSERT_EQ(1u, allowlist.blocks().size());
  EXPECT_EQ(10u, allowlist.blocks()[0].prefix_length);
}

TEST_F(KOMachinesPrefsTest, MachinesRoundTrip) {
  const std::vector<Machine> machines = {
      MakeMachine("a", "100.64.0.10", 5900),
      MakeMachine("b", "mac.netbird.example", 5901)};
  SetMachines(prefs_, machines);
  EXPECT_EQ(machines, GetMachines(prefs_));

  std::optional<Machine> found = FindMachine(prefs_, "b");
  ASSERT_TRUE(found);
  EXPECT_EQ("mac.netbird.example", found->host);
  EXPECT_FALSE(FindMachine(prefs_, "missing"));
}

TEST_F(KOMachinesPrefsTest, InvalidStoredEntriesAreIgnored) {
  base::ListValue list;
  list.Append(MakeMachine("a", "100.64.0.10", 5900).ToValue());
  list.Append(base::DictValue().Set("id", "b").Set("host", "x"));
  prefs_.SetList(prefs::kMachines, std::move(list));
  ASSERT_EQ(1u, GetMachines(prefs_).size());
  EXPECT_FALSE(FindMachine(prefs_, "b"));
}

TEST_F(KOMachinesPrefsTest, AllowlistSkipsPublicEntries) {
  SetAllowedCidrs(prefs_, {"100.64.0.0/10", "0.0.0.0/0", "192.168.1.0/24"});
  std::vector<std::string> rejected;
  const AddressAllowlist allowlist = GetAllowlist(prefs_, &rejected);
  EXPECT_EQ(2u, allowlist.blocks().size());
  EXPECT_EQ(std::vector<std::string>{"0.0.0.0/0"}, rejected);
}

TEST_F(KOMachinesPrefsTest, EmptyAllowlistStaysEmpty) {
  SetAllowedCidrs(prefs_, {});
  EXPECT_TRUE(GetAllowedCidrs(prefs_).empty());
  EXPECT_TRUE(GetAllowlist(prefs_).empty());
}

TEST_F(KOMachinesPrefsTest, MergeSkipsKnownIdsAndEndpoints) {
  std::vector<Machine> machines = {MakeMachine("a", "100.64.0.10", 5900)};
  const std::vector<Machine> imported = {
      MakeMachine("a", "100.64.0.20", 5900),  // Same id.
      MakeMachine("b", "100.64.0.10", 5900),  // Same host:port.
      MakeMachine("c", "100.64.0.10", 5901),
      MakeMachine("c", "100.64.0.30", 5900),  // Same id as the one before.
  };
  EXPECT_EQ(1u, MergeMachines(machines, imported));
  ASSERT_EQ(2u, machines.size());
  EXPECT_EQ("c", machines[1].id);
  EXPECT_EQ(5901, machines[1].port);
}

}  // namespace ko_machines
