// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/ko/machines/core/machines_config.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ko_machines {

namespace {

constexpr char kSampleConfig[] = R"({
  // Comments and trailing commas are allowed.
  "allowed_cidrs": ["100.64.0.0/10", "192.168.1.0/24",],
  "machines": [
    {"id": "studio", "name": "Studio Mac", "host": "100.64.0.10",
     "kind": "macos-screen-sharing"},
    {"name": "Build box", "host": "build.netbird.example", "port": 5901,
     "kind": "vnc"},
    {"name": "Broken", "host": "bad host"},
  ],
})";

}  // namespace

TEST(KOMachinesConfigTest, ParsesSample) {
  std::optional<MachinesConfig> config = ParseMachinesConfig(kSampleConfig);
  ASSERT_TRUE(config);
  ASSERT_EQ(2u, config->machines.size());
  EXPECT_EQ("studio", config->machines[0].id);
  EXPECT_EQ(kDefaultPort, config->machines[0].port);
  EXPECT_EQ("build.netbird.example", config->machines[1].host);
  EXPECT_EQ(5901, config->machines[1].port);
  EXPECT_EQ(MachineKind::kVnc, config->machines[1].kind);
  EXPECT_EQ(1u, config->skipped);
  ASSERT_TRUE(config->allowed_cidrs);
  EXPECT_EQ((std::vector<std::string>{"100.64.0.0/10", "192.168.1.0/24"}),
            *config->allowed_cidrs);
}

TEST(KOMachinesConfigTest, EitherKeyIsEnough) {
  std::optional<MachinesConfig> only_machines =
      ParseMachinesConfig(R"({"machines": []})");
  ASSERT_TRUE(only_machines);
  EXPECT_FALSE(only_machines->allowed_cidrs);

  std::optional<MachinesConfig> only_cidrs =
      ParseMachinesConfig(R"({"allowed_cidrs": ["10.0.0.0/24"]})");
  ASSERT_TRUE(only_cidrs);
  EXPECT_TRUE(only_cidrs->machines.empty());
  ASSERT_TRUE(only_cidrs->allowed_cidrs);
  EXPECT_EQ(1u, only_cidrs->allowed_cidrs->size());
}

TEST(KOMachinesConfigTest, RejectsWrongShapes) {
  EXPECT_FALSE(ParseMachinesConfig(""));
  EXPECT_FALSE(ParseMachinesConfig("not json"));
  EXPECT_FALSE(ParseMachinesConfig("[]"));
  EXPECT_FALSE(ParseMachinesConfig("{}"));
  EXPECT_FALSE(ParseMachinesConfig(R"({"machines": {}})"));
  EXPECT_FALSE(ParseMachinesConfig(R"({"allowed_cidrs": "10.0.0.0/8"})"));
  EXPECT_FALSE(ParseMachinesConfig(R"({"allowed_cidrs": [10]})"));
}

class KOMachinesConfigFileTest : public testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(temp_dir_.CreateUniqueTempDir()); }

  base::FilePath config_path() const {
    return temp_dir_.GetPath().Append(kMachinesConfigFileName);
  }

  void WriteConfig(std::string_view contents) {
    ASSERT_TRUE(base::WriteFile(config_path(), contents));
#if BUILDFLAG(IS_POSIX)
    ASSERT_TRUE(base::SetPosixFilePermissions(config_path(), 0600));
#endif
  }

  base::ScopedTempDir temp_dir_;
};

TEST_F(KOMachinesConfigFileTest, ReadsFile) {
  WriteConfig(kSampleConfig);
  ReadConfigResult result = ReadMachinesConfig(temp_dir_.GetPath());
  EXPECT_EQ(ReadConfigStatus::kOk, result.status);
  EXPECT_EQ(2u, result.config.machines.size());
}

TEST_F(KOMachinesConfigFileTest, MissingFile) {
  EXPECT_EQ(ReadConfigStatus::kNotFound,
            ReadMachinesConfig(temp_dir_.GetPath()).status);
  EXPECT_EQ(ReadConfigStatus::kNotFound,
            ReadMachinesConfig(base::FilePath()).status);
}

TEST_F(KOMachinesConfigFileTest, InvalidContents) {
  WriteConfig("{\"machines\": ");
  EXPECT_EQ(ReadConfigStatus::kInvalid,
            ReadMachinesConfig(temp_dir_.GetPath()).status);
}

TEST_F(KOMachinesConfigFileTest, TooLarge) {
  WriteConfig(std::string(kMaxMachinesConfigSize + 1, ' '));
  EXPECT_EQ(ReadConfigStatus::kInvalid,
            ReadMachinesConfig(temp_dir_.GetPath()).status);
}

#if BUILDFLAG(IS_POSIX)
TEST_F(KOMachinesConfigFileTest, RefusesFileWritableByOthers) {
  WriteConfig(kSampleConfig);
  ASSERT_TRUE(base::SetPosixFilePermissions(config_path(), 0666));
  EXPECT_EQ(ReadConfigStatus::kInsecure,
            ReadMachinesConfig(temp_dir_.GetPath()).status);

  ASSERT_TRUE(base::SetPosixFilePermissions(config_path(), 0620));
  EXPECT_EQ(ReadConfigStatus::kInsecure,
            ReadMachinesConfig(temp_dir_.GetPath()).status);

  // Readable by others is fine; the file holds no secrets.
  ASSERT_TRUE(base::SetPosixFilePermissions(config_path(), 0644));
  EXPECT_EQ(ReadConfigStatus::kOk,
            ReadMachinesConfig(temp_dir_.GetPath()).status);
}

TEST_F(KOMachinesConfigFileTest, RefusesNonRegularFile) {
  ASSERT_TRUE(base::CreateDirectory(config_path()));
  EXPECT_EQ(ReadConfigStatus::kInsecure,
            ReadMachinesConfig(temp_dir_.GetPath()).status);
}
#endif  // BUILDFLAG(IS_POSIX)

}  // namespace ko_machines
