/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef BRAVE_COMPONENTS_KO_COMMON_AGENTD_CONFIG_H_
#define BRAVE_COMPONENTS_KO_COMMON_AGENTD_CONFIG_H_

#include <stddef.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "base/files/file_path.h"

// How the browser finds ko-agentd: the files it writes to the data dir
// (ko::GetDataDir()). Shared by the K-O Assistant and chrome://ko-workspace;
// see ko/docs/agentd.md.
namespace ko {

inline constexpr char kAgentdTokenFileName[] = "agentd.token";
inline constexpr char kAgentdPortFileName[] = "agentd.port";
inline constexpr uint16_t kAgentdDefaultPort = 47821;
// agentd's token is 32 random bytes in unpadded base64url.
inline constexpr size_t kAgentdTokenLength = 43;

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
  AgentdConfig(AgentdConfig&&);
  AgentdConfig& operator=(AgentdConfig&&);
  ~AgentdConfig();

  // From agentd.port if it is private and valid, else kAgentdDefaultPort.
  uint16_t port = kAgentdDefaultPort;
  // Empty unless `token_status` is kOk.
  std::string token;
  AgentdTokenStatus token_status = AgentdTokenStatus::kMissing;

  // "http://127.0.0.1:<port>".
  std::string GetBaseUrl() const;
};

// Blocking. Reads agentd.port and agentd.token from `data_dir`; both must be
// regular files owned by the user with no group/other access. Never logs the
// token.
AgentdConfig ReadAgentdConfig(const base::FilePath& data_dir);

// The trimmed token if `contents` is kAgentdTokenLength base64url characters
// ([A-Za-z0-9_-]), optionally surrounded by ASCII whitespace.
std::optional<std::string> ParseAgentdToken(std::string_view contents);
// 1-65535, optionally surrounded by ASCII whitespace.
std::optional<uint16_t> ParseAgentdPort(std::string_view contents);

enum class PrivateFileError { kMissing, kInsecure, kTooLarge, kReadFailed };

// Blocking. Reads a regular, non-symlink file owned by the current user.
// With `allow_group_or_world_read` false, any group/other permission bit is
// refused; group/other write is always refused. Fails closed where POSIX
// permissions are unavailable.
std::optional<std::string> ReadUserFile(const base::FilePath& path,
                                        size_t max_size,
                                        bool allow_group_or_world_read,
                                        PrivateFileError* error);

}  // namespace ko

#endif  // BRAVE_COMPONENTS_KO_COMMON_AGENTD_CONFIG_H_
