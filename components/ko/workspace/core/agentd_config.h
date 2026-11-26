// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef BRAVE_COMPONENTS_KO_WORKSPACE_CORE_AGENTD_CONFIG_H_
#define BRAVE_COMPONENTS_KO_WORKSPACE_CORE_AGENTD_CONFIG_H_

#include <string>

#include "base/files/file_path.h"
#include "brave/components/ko/common/agentd_config.h"

// Reading agentd's token and port lives in //brave/components/ko/common so the
// Assistant shares it.
namespace ko_workspace {

inline constexpr char kWorkspaceAppsFileName[] = "workspace_apps.json";

// ~/Library/Application Support/K-O on macOS; $XDG_DATA_HOME/k-o, falling
// back to ~/.local/share/k-o, on Linux. Empty elsewhere.
base::FilePath GetKoDataDir();

// Builds the agentd.js response that hands the page its loadTimeData values.
std::string BuildAgentdScript(const ko::AgentdConfig& config);

}  // namespace ko_workspace

#endif  // BRAVE_COMPONENTS_KO_WORKSPACE_CORE_AGENTD_CONFIG_H_
