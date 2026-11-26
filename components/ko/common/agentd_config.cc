/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "brave/components/ko/common/agentd_config.h"

#include <algorithm>
#include <utility>

#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/logging.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_POSIX)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ko {

namespace {

constexpr size_t kMaxTokenFileSize = 1024;
constexpr size_t kMaxPortFileSize = 32;

bool IsBase64UrlChar(char c) {
  return base::IsAsciiAlphaNumeric(c) || c == '-' || c == '_';
}

}  // namespace

AgentdConfig::AgentdConfig() = default;
AgentdConfig::AgentdConfig(const AgentdConfig&) = default;
AgentdConfig& AgentdConfig::operator=(const AgentdConfig&) = default;
AgentdConfig::AgentdConfig(AgentdConfig&&) = default;
AgentdConfig& AgentdConfig::operator=(AgentdConfig&&) = default;
AgentdConfig::~AgentdConfig() = default;

std::string AgentdConfig::GetBaseUrl() const {
  return base::StrCat({"http://127.0.0.1:", base::NumberToString(port)});
}

std::optional<std::string> ParseAgentdToken(std::string_view contents) {
  const std::string_view token =
      base::TrimWhitespaceASCII(contents, base::TRIM_ALL);
  if (token.size() != kAgentdTokenLength ||
      !std::ranges::all_of(token, IsBase64UrlChar)) {
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
  // Check the opened file itself so a swap after lstat() can't matter.
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

  // A port file others can write could point the token at another local
  // listener, so it must be as private as the token.
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

}  // namespace ko
