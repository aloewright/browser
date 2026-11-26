// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/ko/workspace/core/agentd_config.h"

#include <string>

#include "testing/gtest/include/gtest/gtest.h"

namespace ko_workspace {

namespace {

// Placeholder in agentd's format, not a real token.
constexpr char kToken[] = "test_token_not_a_real_secret_0123456789-ABC";

}  // namespace

TEST(KoWorkspaceAgentdScriptTest, CarriesConfigOnlyWhenTokenIsUsable) {
  ko::AgentdConfig config;
  config.port = 47999;
  config.token = kToken;
  config.token_status = ko::AgentdTokenStatus::kOk;
  std::string script = BuildAgentdScript(config);
  EXPECT_TRUE(script.starts_with("loadTimeData.overrideValues({"));
  EXPECT_NE(script.find("http://127.0.0.1:47999"), std::string::npos);
  EXPECT_NE(script.find(kToken), std::string::npos);
  EXPECT_NE(script.find("\"agentdTokenStatus\":\"ok\""), std::string::npos);

  config.token_status = ko::AgentdTokenStatus::kInsecure;
  script = BuildAgentdScript(config);
  EXPECT_EQ(script.find(kToken), std::string::npos);
  EXPECT_NE(script.find("\"agentdTokenStatus\":\"insecure\""),
            std::string::npos);
}

TEST(KoWorkspaceAgentdScriptTest, DefaultConfigPointsAtDefaultPort) {
  const std::string script = BuildAgentdScript(ko::AgentdConfig());
  EXPECT_NE(script.find("http://127.0.0.1:47821"), std::string::npos);
  EXPECT_NE(script.find("\"agentdTokenStatus\":\"missing\""),
            std::string::npos);
}

}  // namespace ko_workspace
