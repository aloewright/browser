// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/ko/machines/core/machines_config.h"

#include <memory>
#include <utility>

#include "base/base_paths.h"
#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/json/json_reader.h"
#include "base/path_service.h"
#include "base/values.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_POSIX)
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if BUILDFLAG(IS_LINUX)
#include "base/environment.h"
#include "base/files/file_util.h"
#include "base/nix/xdg_util.h"
#endif

namespace ko_machines {

namespace {

constexpr char kMachinesKey[] = "machines";
constexpr char kAllowedCidrsKey[] = "allowed_cidrs";
constexpr size_t kMaxCidrs = 32;
constexpr size_t kMaxCidrLength = 64;

#if BUILDFLAG(IS_POSIX)
// Only the user may be able to change the file: it can widen the allowlist.
bool IsSafeConfigFile(const struct stat& st) {
  return S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
         (st.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}
#endif

}  // namespace

base::FilePath GetKODataDir() {
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

MachinesConfig::MachinesConfig() = default;
MachinesConfig::MachinesConfig(MachinesConfig&&) = default;
MachinesConfig& MachinesConfig::operator=(MachinesConfig&&) = default;
MachinesConfig::~MachinesConfig() = default;

ReadConfigResult::ReadConfigResult() = default;
ReadConfigResult::ReadConfigResult(ReadConfigResult&&) = default;
ReadConfigResult& ReadConfigResult::operator=(ReadConfigResult&&) = default;
ReadConfigResult::~ReadConfigResult() = default;

std::optional<MachinesConfig> ParseMachinesConfig(std::string_view json) {
  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_CHROMIUM_EXTENSIONS |
                                           base::JSON_ALLOW_TRAILING_COMMAS);
  if (!root) {
    return std::nullopt;
  }
  const base::Value* machines = root->Find(kMachinesKey);
  const base::Value* cidrs = root->Find(kAllowedCidrsKey);
  if (!machines && !cidrs) {
    return std::nullopt;
  }

  MachinesConfig config;
  if (machines) {
    const base::ListValue* list = machines->GetIfList();
    if (!list) {
      return std::nullopt;
    }
    config.machines = MachinesFromList(*list, &config.skipped);
  }
  if (cidrs) {
    const base::ListValue* list = cidrs->GetIfList();
    if (!list || list->size() > kMaxCidrs) {
      return std::nullopt;
    }
    std::vector<std::string> strings;
    for (const base::Value& value : *list) {
      const std::string* cidr = value.GetIfString();
      if (!cidr || cidr->size() > kMaxCidrLength) {
        return std::nullopt;
      }
      strings.push_back(*cidr);
    }
    config.allowed_cidrs = std::move(strings);
  }
  return config;
}

ReadConfigResult ReadMachinesConfig(const base::FilePath& data_dir) {
  ReadConfigResult result;
  if (data_dir.empty()) {
    return result;
  }
  const base::FilePath path = data_dir.Append(kMachinesConfigFileName);

#if BUILDFLAG(IS_POSIX)
  // Check before opening so a FIFO cannot block the open.
  struct stat path_stat;
  if (stat(path.value().c_str(), &path_stat) != 0) {
    result.status = errno == ENOENT ? ReadConfigStatus::kNotFound
                                    : ReadConfigStatus::kInvalid;
    return result;
  }
  if (!IsSafeConfigFile(path_stat)) {
    result.status = ReadConfigStatus::kInsecure;
    return result;
  }
#endif

  base::File file(path, base::File::FLAG_OPEN | base::File::FLAG_READ);
  if (!file.IsValid()) {
    result.status = file.error_details() == base::File::FILE_ERROR_NOT_FOUND
                        ? ReadConfigStatus::kNotFound
                        : ReadConfigStatus::kInvalid;
    return result;
  }

#if BUILDFLAG(IS_POSIX)
  // Re-check the file that was actually opened.
  struct stat file_stat;
  if (fstat(file.GetPlatformFile(), &file_stat) != 0 ||
      !IsSafeConfigFile(file_stat)) {
    result.status = ReadConfigStatus::kInsecure;
    return result;
  }
#endif

  const int64_t length = file.GetLength();
  if (length < 0 || length > static_cast<int64_t>(kMaxMachinesConfigSize)) {
    result.status = ReadConfigStatus::kInvalid;
    return result;
  }
  std::string contents(static_cast<size_t>(length), '\0');
  std::optional<size_t> read =
      file.Read(0, base::as_writable_byte_span(contents));
  if (!read || *read != contents.size()) {
    result.status = ReadConfigStatus::kInvalid;
    return result;
  }

  std::optional<MachinesConfig> config = ParseMachinesConfig(contents);
  if (!config) {
    result.status = ReadConfigStatus::kInvalid;
    return result;
  }
  result.status = ReadConfigStatus::kOk;
  result.config = std::move(*config);
  return result;
}

}  // namespace ko_machines
