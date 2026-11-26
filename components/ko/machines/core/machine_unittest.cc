// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/ko/machines/core/machine.h"

#include <optional>
#include <string>

#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ko_machines {

namespace {

base::DictValue ValidMachineDict() {
  return base::DictValue()
      .Set("id", "studio")
      .Set("name", "Studio Mac")
      .Set("host", "100.64.0.10")
      .Set("port", 5900)
      .Set("kind", "macos-screen-sharing");
}

}  // namespace

TEST(KOMachineTest, CanonicalizesHosts) {
  EXPECT_EQ("mac-mini.netbird.example",
            CanonicalizeMachineHost("Mac-Mini.NetBird.example"));
  EXPECT_EQ("100.64.0.10", CanonicalizeMachineHost("100.64.0.10"));
  EXPECT_EQ("100.64.0.10", CanonicalizeMachineHost("  100.64.0.10 "));
  EXPECT_EQ("[fd00::1]", CanonicalizeMachineHost("fd00::1"));
  EXPECT_EQ("[fd00::1]", CanonicalizeMachineHost("[fd00:0::1]"));
  EXPECT_EQ("localhost", CanonicalizeMachineHost("LOCALHOST"));
}

TEST(KOMachineTest, RejectsHostsWithMoreThanAHost) {
  EXPECT_FALSE(CanonicalizeMachineHost(""));
  EXPECT_FALSE(CanonicalizeMachineHost("   "));
  EXPECT_FALSE(CanonicalizeMachineHost("host.example:5900"));
  EXPECT_FALSE(CanonicalizeMachineHost("[fd00::1]:5900"));
  EXPECT_FALSE(CanonicalizeMachineHost("vnc://host.example"));
  EXPECT_FALSE(CanonicalizeMachineHost("user@host.example"));
  EXPECT_FALSE(CanonicalizeMachineHost("host.example/path"));
  EXPECT_FALSE(CanonicalizeMachineHost("two words"));
  EXPECT_FALSE(CanonicalizeMachineHost("100.64.0.256"));
  EXPECT_FALSE(CanonicalizeMachineHost(std::string(300, 'a')));
}

TEST(KOMachineTest, NormalizesNames) {
  EXPECT_EQ("Studio Mac", NormalizeMachineName("  Studio Mac  "));
  EXPECT_EQ("B\xC3\xBCro", NormalizeMachineName("B\xC3\xBCro"));
  EXPECT_FALSE(NormalizeMachineName(""));
  EXPECT_FALSE(NormalizeMachineName("   "));
  EXPECT_FALSE(NormalizeMachineName("tab\tinside"));
  EXPECT_FALSE(NormalizeMachineName("bad\xFF"));
  EXPECT_FALSE(NormalizeMachineName(std::string(kMaxNameLength + 1, 'a')));
  EXPECT_TRUE(NormalizeMachineName(std::string(kMaxNameLength, 'a')));
}

TEST(KOMachineTest, ValidatesIds) {
  EXPECT_TRUE(IsValidMachineId("studio"));
  EXPECT_TRUE(IsValidMachineId("Build_Box-2"));
  EXPECT_TRUE(IsValidMachineId(GenerateMachineId()));
  EXPECT_FALSE(IsValidMachineId(""));
  EXPECT_FALSE(IsValidMachineId("has space"));
  EXPECT_FALSE(IsValidMachineId("dot.id"));
  EXPECT_FALSE(IsValidMachineId("../x"));
  EXPECT_FALSE(IsValidMachineId(std::string(kMaxIdLength + 1, 'a')));
}

TEST(KOMachineTest, ValidatesPorts) {
  EXPECT_EQ(5900, ValidateMachinePort(5900));
  EXPECT_EQ(1, ValidateMachinePort(1));
  EXPECT_EQ(65535, ValidateMachinePort(65535));
  EXPECT_FALSE(ValidateMachinePort(0));
  EXPECT_FALSE(ValidateMachinePort(-1));
  EXPECT_FALSE(ValidateMachinePort(65536));
}

TEST(KOMachineTest, KindStrings) {
  EXPECT_EQ("macos-screen-sharing",
            MachineKindToString(MachineKind::kScreenSharing));
  EXPECT_EQ("vnc", MachineKindToString(MachineKind::kVnc));
  EXPECT_EQ(MachineKind::kVnc, MachineKindFromString("vnc"));
  EXPECT_FALSE(MachineKindFromString("VNC"));
  EXPECT_FALSE(MachineKindFromString("rdp"));
}

TEST(KOMachineTest, FromValueRoundTrips) {
  std::optional<Machine> machine = Machine::FromValue(ValidMachineDict());
  ASSERT_TRUE(machine);
  EXPECT_EQ("studio", machine->id);
  EXPECT_EQ("Studio Mac", machine->name);
  EXPECT_EQ("100.64.0.10", machine->host);
  EXPECT_EQ(5900, machine->port);
  EXPECT_EQ(MachineKind::kScreenSharing, machine->kind);

  EXPECT_EQ(ValidMachineDict(), machine->ToValue());
  EXPECT_EQ(machine, Machine::FromValue(machine->ToValue()));
}

TEST(KOMachineTest, FromValueFillsDefaults) {
  std::optional<Machine> machine = Machine::FromValue(
      base::DictValue().Set("name", "Box").Set("host", "box.example"));
  ASSERT_TRUE(machine);
  EXPECT_TRUE(IsValidMachineId(machine->id));
  EXPECT_EQ(kDefaultPort, machine->port);
  EXPECT_EQ(MachineKind::kScreenSharing, machine->kind);
}

TEST(KOMachineTest, FromValueRejectsInvalidFields) {
  EXPECT_FALSE(Machine::FromValue(ValidMachineDict().Set("id", "bad id")));
  EXPECT_FALSE(Machine::FromValue(ValidMachineDict().Set("id", 5)));
  EXPECT_FALSE(Machine::FromValue(ValidMachineDict().Set("name", "")));
  EXPECT_FALSE(Machine::FromValue(ValidMachineDict().Set("host", "a:1")));
  EXPECT_FALSE(Machine::FromValue(ValidMachineDict().Set("port", 0)));
  EXPECT_FALSE(Machine::FromValue(ValidMachineDict().Set("port", 70000)));
  EXPECT_FALSE(Machine::FromValue(ValidMachineDict().Set("port", "5900")));
  EXPECT_FALSE(Machine::FromValue(ValidMachineDict().Set("kind", "rdp")));

  base::DictValue no_name = ValidMachineDict();
  no_name.Remove("name");
  EXPECT_FALSE(Machine::FromValue(no_name));
  base::DictValue no_host = ValidMachineDict();
  no_host.Remove("host");
  EXPECT_FALSE(Machine::FromValue(no_host));
}

TEST(KOMachineTest, ListSkipsInvalidAndDuplicateEntries) {
  base::ListValue list;
  list.Append(ValidMachineDict());
  list.Append(ValidMachineDict().Set("name", "Duplicate id"));
  list.Append(ValidMachineDict().Set("id", "box").Set("host", "bad host"));
  list.Append("not a dict");
  list.Append(ValidMachineDict().Set("id", "vnc-box").Set("kind", "vnc"));

  size_t skipped = 0;
  std::vector<Machine> machines = MachinesFromList(list, &skipped);
  ASSERT_EQ(2u, machines.size());
  EXPECT_EQ("studio", machines[0].id);
  EXPECT_EQ("vnc-box", machines[1].id);
  EXPECT_EQ(MachineKind::kVnc, machines[1].kind);
  EXPECT_EQ(3u, skipped);

  EXPECT_EQ(machines, MachinesFromList(MachinesToList(machines)));
}

TEST(KOMachineTest, ListIsCapped) {
  base::ListValue list;
  for (size_t i = 0; i < kMaxMachines + 5; ++i) {
    list.Append(base::DictValue()
                    .Set("name", "Box")
                    .Set("host", "box.example")
                    .Set("port", static_cast<int>(5900 + i)));
  }
  size_t skipped = 0;
  EXPECT_EQ(kMaxMachines, MachinesFromList(list, &skipped).size());
  EXPECT_EQ(5u, skipped);
}

}  // namespace ko_machines
