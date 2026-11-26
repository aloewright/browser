// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/ko/workspace/core/agentd_config.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "base/base_paths.h"
#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_POSIX)
#include <sys/stat.h>
#include <unistd.h>
#endif

#if BUILDFLAG(IS_LINUX)
#include "base/environment.h"
#include "base/files/file_util.h"
#include "base/nix/xdg_util.h"
#endif

namespace ko_workspace {

namespace {

constexpr size_t kMaxTokenFileSize = 1024;
constexpr size_t kMaxPortFileSize = 32;
constexpr size_t kMinTokenLength = 16;
constexpr size_t kMaxTokenLength = 256;

bool IsTokenChar(char c) {
  return base::IsAsciiAlphaNumeric(c) || c == '-' || c == '_';
}

}  // namespace

base::FilePath GetKoDataDir() {
#if BUILDFLAG(IS_MAC)
  base::FilePath app_support;
  if (!base::PathService::Get(base::DIR_APP_DATA, &app_support)) {
    return base::FilePath();
  }
  return app_support.Append(FILE_PATH_LITERAL("K-O"));
#elif BUILDFLAG(IS_LINUX)
  std::unique_ptr<base::Environment> env = base::Environment::Create();
  base::FilePath data_home =
      base::nix::GetXDGDirectory(env.get(), "XDG_DATA_HOME", ".local/share");
  if (data_home.empty() || !data_home.IsAbsolute()) {
    const base::FilePath home = base::GetHomeDir();
    if (home.empty()) {
      return base::FilePath();
    }
    data_home = home.Append(".local").Append("share");
  }
  return data_home.Append("k-o");
#else
  return base::FilePath();
#endif
}

AgentdConfig::AgentdConfig() = default;
AgentdConfig::AgentdConfig(const AgentdConfig&) = default;
AgentdConfig& AgentdConfig::operator=(const AgentdConfig&) = default;
AgentdConfig::~AgentdConfig() = default;

std::string AgentdConfig::GetBaseUrl() const {
  return base::StrCat({"http://127.0.0.1:", base::NumberToString(port)});
}

std::optional<std::string> ParseAgentdToken(std::string_view contents) {
  const std::string_view token =
      base::TrimWhitespaceASCII(contents, base::TRIM_ALL);
  if (token.size() < kMinTokenLength || token.size() > kMaxTokenLength ||
      !std::ranges::all_of(token, IsTokenChar)) {
    return std::nullopt;
  }
  return std::string(token);
}

std::optional<uint16_t> ParseAgentdPort(std::string_view contents) {
  unsigned port = 0;
  if (!base::StringToUint(base::TrimWhitespaceASCII(contents, base::TRIM_ALL),
                          &port) ||
      port == 0 || port > 65535) {
    return std::nullopt;
  }
  return static_cast<uint16_t>(port);
}

std::optional<std::string> ReadUserFile(const base::FilePath& path,
                                        size_t max_size,
                                        bool allow_group_or_world_read,
                                        PrivateFileError* error) {
  PrivateFileError ignored;
  if (!error) {
    error = &ignored;
  }
#if BUILDFLAG(IS_POSIX)
  struct stat link_info;
  if (lstat(path.value().c_str(), &link_info) != 0) {
    *error = PrivateFileError::kMissing;
    return std::nullopt;
  }
  // Refuses symlinks, directories, FIFOs and devices.
  if (!S_ISREG(link_info.st_mode)) {
    *error = PrivateFileError::kInsecure;
    return std::nullopt;
  }
  base::File file(path, base::File::FLAG_OPEN | base::File::FLAG_READ);
  if (!file.IsValid()) {
    *error = PrivateFileError::kReadFailed;
    return std::nullopt;
  }
  struct stat info;
  if (fstat(file.GetPlatformFile(), &info) != 0 ||
      info.st_ino != link_info.st_ino || info.st_dev != link_info.st_dev) {
    *error = PrivateFileError::kInsecure;
    return std::nullopt;
  }
  const mode_t forbidden =
      allow_group_or_world_read ? (S_IWGRP | S_IWOTH) : (S_IRWXG | S_IRWXO);
  if (!S_ISREG(info.st_mode) || info.st_uid != geteuid() ||
      (info.st_mode & forbidden) != 0) {
    *error = PrivateFileError::kInsecure;
    return std::nullopt;
  }
  if (info.st_size < 0 || static_cast<size_t>(info.st_size) > max_size) {
    *error = PrivateFileError::kTooLarge;
    return std::nullopt;
  }
  std::string contents(static_cast<size_t>(info.st_size), '\0');
  if (!file.ReadAndCheck(0, base::as_writable_byte_span(contents))) {
    *error = PrivateFileError::kReadFailed;
    return std::nullopt;
  }
  return contents;
#else
  // Permissions cannot be checked here: fail closed.
  *error = PrivateFileError::kInsecure;
  return std::nullopt;
#endif
}

AgentdConfig ReadAgentdConfig(const base::FilePath& data_dir) {
  AgentdConfig config;
  if (data_dir.empty()) {
    return config;
  }

  // A port file others can write could point the page, and its token, at
  // another local listener, so it must be as private as the token.
  if (auto contents = ReadUserFile(
          data_dir.AppendASCII(kAgentdPortFileName), kMaxPortFileSize,
          /*allow_group_or_world_read=*/false, nullptr)) {
    if (auto port = ParseAgentdPort(*contents)) {
      config.port = *port;
    }
  }

  PrivateFileError error = PrivateFileError::kMissing;
  auto contents = ReadUserFile(data_dir.AppendASCII(kAgentdTokenFileName),
                               kMaxTokenFileSize,
                               /*allow_group_or_world_read=*/false, &error);
  if (!contents) {
    config.token_status = error == PrivateFileError::kMissing
                              ? AgentdTokenStatus::kMissing
                              : AgentdTokenStatus::kInsecure;
    if (config.token_status == AgentdTokenStatus::kInsecure) {
      LOG(ERROR) << "Ignoring agentd token file with unsafe permissions";
    }
    return config;
  }
  if (auto token = ParseAgentdToken(*contents)) {
    config.token = std::move(*token);
    config.token_status = AgentdTokenStatus::kOk;
  } else {
    config.token_status = AgentdTokenStatus::kInvalid;
  }
  return config;
}

std::string BuildAgentdScript(const AgentdConfig& config) {
  std::string_view status = "missing";
  switch (config.token_status) {
    case AgentdTokenStatus::kOk:
      status = "ok";
      break;
    case AgentdTokenStatus::kMissing:
      status = "missing";
      break;
    case AgentdTokenStatus::kInsecure:
      status = "insecure";
      break;
    case AgentdTokenStatus::kInvalid:
      status = "invalid";
      break;
  }
  const std::string json =
      base::WriteJson(
          base::DictValue()
              .Set("agentdBaseUrl", config.GetBaseUrl())
              .Set("agentdToken", config.token_status == AgentdTokenStatus::kOk
                                      ? config.token
                                      : std::string())
              .Set("agentdTokenStatus", status))
          .value_or("{}");
  return base::StrCat({"loadTimeData.overrideValues(", json, ");\n"});
}

}  // namespace ko_workspace
