// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/ko/workspace/core/agentd_config.h"

#include <string_view>

#include "base/json/json_writer.h"
#include "base/strings/strcat.h"
#include "base/values.h"
#include "brave/components/ko/common/ko_paths.h"

namespace ko_workspace {

base::FilePath GetKoDataDir() {
  return ::ko::GetDataDir();
}

std::string BuildAgentdScript(const ko::AgentdConfig& config) {
  std::string_view status = "missing";
  switch (config.token_status) {
    case ko::AgentdTokenStatus::kOk:
      status = "ok";
      break;
    case ko::AgentdTokenStatus::kMissing:
      status = "missing";
      break;
    case ko::AgentdTokenStatus::kInsecure:
      status = "insecure";
      break;
    case ko::AgentdTokenStatus::kInvalid:
      status = "invalid";
      break;
  }
  const std::string json =
      base::WriteJson(base::DictValue()
                          .Set("agentdBaseUrl", config.GetBaseUrl())
                          .Set("agentdToken",
                               config.token_status == ko::AgentdTokenStatus::kOk
                                   ? config.token
                                   : std::string())
                          .Set("agentdTokenStatus", status))
          .value_or("{}");
  return base::StrCat({"loadTimeData.overrideValues(", json, ");\n"});
}

}  // namespace ko_workspace
