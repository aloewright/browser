// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef BRAVE_COMPONENTS_KO_WORKSPACE_CORE_AGENTD_CONFIG_H_
#define BRAVE_COMPONENTS_KO_WORKSPACE_CORE_AGENTD_CONFIG_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "base/files/file_path.h"

namespace ko_workspace {

// Shared with ko-agentd, see ko/docs/agentd.md.
inline constexpr char kAgentdTokenFileName[] = "agentd.token";
inline constexpr char kAgentdPortFileName[] = "agentd.port";
inline constexpr char kWorkspaceAppsFileName[] = "workspace_apps.json";
inline constexpr uint16_t kDefaultAgentdPort = 47821;

// ~/Library/Application Support/K-O on macOS; $XDG_DATA_HOME/k-o, falling
// back to ~/.local/share/k-o, on Linux. Empty elsewhere.
base::FilePath GetKoDataDir();

enum class AgentdTokenStatus {
  kOk,
  kMissing,
  // Not a regular file owned by the user, or readable/writable by others.
  kInsecure,
  kInvalid,
};

struct AgentdConfig {
  AgentdConfig();
  AgentdConfig(const AgentdConfig&);
  AgentdConfig& operator=(const AgentdConfig&);
  ~AgentdConfig();

  uint16_t port = kDefaultAgentdPort;
  // Empty unless `token_status` is kOk.
  std::string token;
  AgentdTokenStatus token_status = AgentdTokenStatus::kMissing;

  // "http://127.0.0.1:<port>".
  std::string GetBaseUrl() const;
};

// Blocking. Reads agentd.port (if private and valid) and agentd.token from
// `data_dir`. Never logs the token.
AgentdConfig ReadAgentdConfig(const base::FilePath& data_dir);

// Returns the trimmed token if it is 16-256 base64url characters.
std::optional<std::string> ParseAgentdToken(std::string_view contents);
std::optional<uint16_t> ParseAgentdPort(std::string_view contents);

enum class PrivateFileError { kMissing, kInsecure, kTooLarge, kReadFailed };

// Blocking. Reads a regular, non-symlink file owned by the current user.
// With `allow_group_or_world_read` false, any group/other permission bit is
// refused; group/other write is always refused.
std::optional<std::string> ReadUserFile(const base::FilePath& path,
                                        size_t max_size,
                                        bool allow_group_or_world_read,
                                        PrivateFileError* error);

// Builds the agentd.js response that hands the page its loadTimeData values.
std::string BuildAgentdScript(const AgentdConfig& config);

}  // namespace ko_workspace

#endif  // BRAVE_COMPONENTS_KO_WORKSPACE_CORE_AGENTD_CONFIG_H_
