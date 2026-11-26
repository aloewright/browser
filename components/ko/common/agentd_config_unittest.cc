// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/ko/common/agentd_config.h"

#include <string>
#include <string_view>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ko {

namespace {

// Placeholder in agentd's format (43 base64url characters), not a real token.
constexpr char kToken[] = "test_token_not_a_real_secret_0123456789-ABC";

std::string Repeat(char c, size_t count) {
  return std::string(count, c);
}

}  // namespace

TEST(KoAgentdConfigTest, ParsesTokensInAgentdFormatOnly) {
  ASSERT_EQ(std::string_view(kToken).size(), 43u);
  EXPECT_EQ(ParseAgentdToken(kToken), kToken);
  // agentd writes the token followed by a newline.
  EXPECT_EQ(ParseAgentdToken(std::string(kToken) + "\n"), kToken);
  EXPECT_EQ(ParseAgentdToken(std::string(" \t") + kToken + "\r\n"), kToken);
  // agentd starts with any base64url token of 32 or more characters.
  EXPECT_TRUE(ParseAgentdToken(Repeat('a', kAgentdTokenMinLength)));
  EXPECT_TRUE(ParseAgentdToken(Repeat('a', 86)));
  EXPECT_TRUE(ParseAgentdToken(Repeat('a', kAgentdTokenMaxLength)));

  EXPECT_FALSE(ParseAgentdToken(""));
  EXPECT_FALSE(ParseAgentdToken("\n"));
  EXPECT_FALSE(ParseAgentdToken("short"));
  EXPECT_FALSE(ParseAgentdToken(Repeat('a', kAgentdTokenMinLength - 1)));
  EXPECT_FALSE(ParseAgentdToken(Repeat('a', kAgentdTokenMaxLength + 1)));
  // Standard base64 and padding are not agentd's alphabet.
  for (const char* last : {"+", "/", "=", ".", "~"}) {
    SCOPED_TRACE(last);
    EXPECT_FALSE(ParseAgentdToken(Repeat('a', 42) + last));
  }
  // Nothing that could break out of a header or a script.
  EXPECT_FALSE(ParseAgentdToken(Repeat('a', 20) + "\r\n" + Repeat('a', 21)));
  EXPECT_FALSE(ParseAgentdToken(Repeat('a', 20) + " " + Repeat('a', 22)));
  EXPECT_FALSE(ParseAgentdToken(Repeat('a', 33) + "\"; alert()"));
}

TEST(KoAgentdConfigTest, ParsesPorts) {
  EXPECT_EQ(ParseAgentdPort("47821\n"), 47821);
  EXPECT_EQ(ParseAgentdPort("1"), 1);
  EXPECT_EQ(ParseAgentdPort("65535"), 65535);
  EXPECT_FALSE(ParseAgentdPort("0"));
  EXPECT_FALSE(ParseAgentdPort("65536"));
  EXPECT_FALSE(ParseAgentdPort("-1"));
  EXPECT_FALSE(ParseAgentdPort("80abc"));
  EXPECT_FALSE(ParseAgentdPort(""));
}

TEST(KoAgentdConfigTest, EmptyDataDirMeansNoToken) {
  const AgentdConfig config = ReadAgentdConfig(base::FilePath());
  EXPECT_EQ(config.token_status, AgentdTokenStatus::kMissing);
  EXPECT_TRUE(config.token.empty());
  EXPECT_EQ(config.port, kAgentdDefaultPort);
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

TEST_F(KoAgentdConfigFileTest, MissingPortFileMeansDefaultPort) {
  Write(kAgentdTokenFileName, kToken, 0600);
  const AgentdConfig config = ReadAgentdConfig(temp_dir_.GetPath());
  EXPECT_EQ(config.token_status, AgentdTokenStatus::kOk);
  EXPECT_EQ(config.port, kAgentdDefaultPort);
}

TEST_F(KoAgentdConfigFileTest, MissingToken) {
  Write(kAgentdPortFileName, "48000\n", 0600);
  const AgentdConfig config = ReadAgentdConfig(temp_dir_.GetPath());
  EXPECT_EQ(config.token_status, AgentdTokenStatus::kMissing);
  EXPECT_TRUE(config.token.empty());
  EXPECT_EQ(config.port, 48000);
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

TEST_F(KoAgentdConfigFileTest, RejectsMalformedOrOversizedToken) {
  Write(kAgentdTokenFileName, "not a token", 0600);
  EXPECT_EQ(ReadAgentdConfig(temp_dir_.GetPath()).token_status,
            AgentdTokenStatus::kInvalid);
  // Fits the file limit but is longer than any token the browser takes.
  Write(kAgentdTokenFileName, Repeat('a', kAgentdTokenMaxLength + 1) + "\n",
        0600);
  EXPECT_EQ(ReadAgentdConfig(temp_dir_.GetPath()).token_status,
            AgentdTokenStatus::kInvalid);
  Write(kAgentdTokenFileName, Repeat('a', 4096), 0600);
  const AgentdConfig config = ReadAgentdConfig(temp_dir_.GetPath());
  EXPECT_NE(config.token_status, AgentdTokenStatus::kOk);
  EXPECT_TRUE(config.token.empty());
}

TEST_F(KoAgentdConfigFileTest, IgnoresLooseOrMalformedPortFile) {
  Write(kAgentdTokenFileName, kToken, 0600);
  for (int mode : {0666, 0644, 0640, 0620}) {
    SCOPED_TRACE(mode);
    Write(kAgentdPortFileName, "48000", mode);
    EXPECT_EQ(ReadAgentdConfig(temp_dir_.GetPath()).port, kAgentdDefaultPort);
  }
  Write(kAgentdPortFileName, "not a port", 0600);
  EXPECT_EQ(ReadAgentdConfig(temp_dir_.GetPath()).port, kAgentdDefaultPort);
}

TEST_F(KoAgentdConfigFileTest, IgnoresSymlinkedPortFile) {
  Write(kAgentdTokenFileName, kToken, 0600);
  const base::FilePath target = Write("other_port", "48000", 0600);
  ASSERT_TRUE(base::CreateSymbolicLink(
      target, temp_dir_.GetPath().AppendASCII(kAgentdPortFileName)));
  EXPECT_EQ(ReadAgentdConfig(temp_dir_.GetPath()).port, kAgentdDefaultPort);
}

TEST_F(KoAgentdConfigFileTest, ReadUserFileLimits) {
  PrivateFileError error;
  const base::FilePath big = Write("big", Repeat('x', 100), 0600);
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

  EXPECT_FALSE(
      ReadUserFile(temp_dir_.GetPath().AppendASCII("none"), 10, true, &error));
  EXPECT_EQ(error, PrivateFileError::kMissing);
}
#endif  // BUILDFLAG(IS_POSIX)

}  // namespace ko
