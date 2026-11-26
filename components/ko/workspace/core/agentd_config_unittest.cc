// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/ko/workspace/core/agentd_config.h"

#include <string>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ko_workspace {

namespace {

// Placeholder, not a real token.
constexpr char kToken[] = "dGVzdC10b2tlbi1ub3QtcmVhbC0xMjM0NTY3ODkw";

}  // namespace

TEST(KoAgentdConfigTest, ParsesTokens) {
  EXPECT_EQ(ParseAgentdToken(kToken), kToken);
  EXPECT_EQ(ParseAgentdToken(std::string(kToken) + "\n"), kToken);
  EXPECT_FALSE(ParseAgentdToken(""));
  EXPECT_FALSE(ParseAgentdToken("short"));
  EXPECT_FALSE(ParseAgentdToken(std::string(257, 'a')));
  EXPECT_FALSE(ParseAgentdToken("abcdefghijklmnop\"; alert(1)//"));
  EXPECT_FALSE(ParseAgentdToken("abcdefghijklmnop qrstuvwx"));
}

TEST(KoAgentdConfigTest, ParsesPorts) {
  EXPECT_EQ(ParseAgentdPort("47821\n"), 47821);
  EXPECT_EQ(ParseAgentdPort("1"), 1);
  EXPECT_FALSE(ParseAgentdPort("0"));
  EXPECT_FALSE(ParseAgentdPort("65536"));
  EXPECT_FALSE(ParseAgentdPort("-1"));
  EXPECT_FALSE(ParseAgentdPort("80abc"));
  EXPECT_FALSE(ParseAgentdPort(""));
}

TEST(KoAgentdConfigTest, ScriptCarriesConfigOnlyWhenTokenIsUsable) {
  AgentdConfig config;
  config.port = 47999;
  config.token = kToken;
  config.token_status = AgentdTokenStatus::kOk;
  std::string script = BuildAgentdScript(config);
  EXPECT_TRUE(script.starts_with("loadTimeData.overrideValues({"));
  EXPECT_NE(script.find("http://127.0.0.1:47999"), std::string::npos);
  EXPECT_NE(script.find(kToken), std::string::npos);
  EXPECT_NE(script.find("\"agentdTokenStatus\":\"ok\""), std::string::npos);

  config.token_status = AgentdTokenStatus::kInsecure;
  script = BuildAgentdScript(config);
  EXPECT_EQ(script.find(kToken), std::string::npos);
  EXPECT_NE(script.find("\"agentdTokenStatus\":\"insecure\""),
            std::string::npos);
}

TEST(KoAgentdConfigTest, EmptyDataDirMeansNoToken) {
  const AgentdConfig config = ReadAgentdConfig(base::FilePath());
  EXPECT_EQ(config.token_status, AgentdTokenStatus::kMissing);
  EXPECT_TRUE(config.token.empty());
  EXPECT_EQ(config.port, kDefaultAgentdPort);
  EXPECT_EQ(config.GetBaseUrl(), "http://127.0.0.1:47821");
}

#if BUILDFLAG(IS_POSIX)
class KoAgentdConfigFileTest : public testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(temp_dir_.CreateUniqueTempDir()); }

  base::FilePath Write(const char* name, std::string_view contents, int mode) {
    const base::FilePath path = temp_dir_.GetPath().AppendASCII(name);
    EXPECT_TRUE(base::WriteFile(path, contents));
    EXPECT_TRUE(base::SetPosixFilePermissions(path, mode));
    return path;
  }

  base::ScopedTempDir temp_dir_;
};

TEST_F(KoAgentdConfigFileTest, ReadsPrivateTokenAndPort) {
  Write(kAgentdTokenFileName, std::string(kToken) + "\n", 0600);
  Write(kAgentdPortFileName, "48000\n", 0600);
  const AgentdConfig config = ReadAgentdConfig(temp_dir_.GetPath());
  EXPECT_EQ(config.token_status, AgentdTokenStatus::kOk);
  EXPECT_EQ(config.token, kToken);
  EXPECT_EQ(config.port, 48000);
  EXPECT_EQ(config.GetBaseUrl(), "http://127.0.0.1:48000");
}

TEST_F(KoAgentdConfigFileTest, MissingToken) {
  const AgentdConfig config = ReadAgentdConfig(temp_dir_.GetPath());
  EXPECT_EQ(config.token_status, AgentdTokenStatus::kMissing);
  EXPECT_TRUE(config.token.empty());
}

TEST_F(KoAgentdConfigFileTest, RefusesLooseTokenPermissions) {
  for (int mode : {0640, 0604, 0620, 0660, 0644}) {
    SCOPED_TRACE(mode);
    Write(kAgentdTokenFileName, kToken, mode);
    const AgentdConfig config = ReadAgentdConfig(temp_dir_.GetPath());
    EXPECT_EQ(config.token_status, AgentdTokenStatus::kInsecure);
    EXPECT_TRUE(config.token.empty());
  }
}

TEST_F(KoAgentdConfigFileTest, RefusesSymlinkedToken) {
  const base::FilePath target = Write("elsewhere", kToken, 0600);
  ASSERT_TRUE(base::CreateSymbolicLink(
      target, temp_dir_.GetPath().AppendASCII(kAgentdTokenFileName)));
  const AgentdConfig config = ReadAgentdConfig(temp_dir_.GetPath());
  EXPECT_EQ(config.token_status, AgentdTokenStatus::kInsecure);
  EXPECT_TRUE(config.token.empty());
}

TEST_F(KoAgentdConfigFileTest, RejectsMalformedToken) {
  Write(kAgentdTokenFileName, "not a token", 0600);
  EXPECT_EQ(ReadAgentdConfig(temp_dir_.GetPath()).token_status,
            AgentdTokenStatus::kInvalid);
}

TEST_F(KoAgentdConfigFileTest, IgnoresLoosePortFile) {
  Write(kAgentdTokenFileName, kToken, 0600);
  Write(kAgentdPortFileName, "48000", 0666);
  EXPECT_EQ(ReadAgentdConfig(temp_dir_.GetPath()).port, kDefaultAgentdPort);
}

TEST_F(KoAgentdConfigFileTest, ReadUserFileLimits) {
  PrivateFileError error;
  const base::FilePath big = Write("big", std::string(100, 'x'), 0600);
  EXPECT_FALSE(ReadUserFile(big, 10, false, &error));
  EXPECT_EQ(error, PrivateFileError::kTooLarge);

  const base::FilePath shared = Write("shared", "ok", 0644);
  EXPECT_FALSE(ReadUserFile(shared, 10, false, &error));
  EXPECT_EQ(error, PrivateFileError::kInsecure);
  EXPECT_EQ(ReadUserFile(shared, 10, true, &error), "ok");

  const base::FilePath writable = Write("writable", "ok", 0646);
  EXPECT_FALSE(ReadUserFile(writable, 10, true, &error));
  EXPECT_EQ(error, PrivateFileError::kInsecure);

  EXPECT_FALSE(ReadUserFile(temp_dir_.GetPath(), 10, true, &error));
  EXPECT_EQ(error, PrivateFileError::kInsecure);
}
#endif  // BUILDFLAG(IS_POSIX)

}  // namespace ko_workspace
