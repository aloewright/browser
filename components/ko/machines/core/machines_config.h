// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef BRAVE_COMPONENTS_KO_MACHINES_CORE_MACHINES_CONFIG_H_
#define BRAVE_COMPONENTS_KO_MACHINES_CORE_MACHINES_CONFIG_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "brave/components/ko/machines/core/machine.h"

namespace ko_machines {

inline constexpr base::FilePath::CharType kMachinesConfigFileName[] =
    FILE_PATH_LITERAL("machines.json");
inline constexpr size_t kMaxMachinesConfigSize = 64 * 1024;

// The K-O data dir: ~/Library/Application Support/K-O on macOS,
// $XDG_DATA_HOME/k-o (else ~/.local/share/k-o) on Linux. Empty elsewhere or if
// it cannot be resolved.
base::FilePath GetKODataDir();

struct MachinesConfig {
  MachinesConfig();
  MachinesConfig(MachinesConfig&&);
  MachinesConfig& operator=(MachinesConfig&&);
  ~MachinesConfig();

  std::vector<Machine> machines;
  // Set only if the file has an "allowed_cidrs" list.
  std::optional<std::vector<std::string>> allowed_cidrs;
  // Invalid machine entries that were dropped.
  size_t skipped = 0;
};

enum class ReadConfigStatus {
  kOk,
  kNotFound,
  // Not a regular file owned by the user, or writable by group or others.
  kInsecure,
  // Too large, unreadable or not valid machines.json.
  kInvalid,
};

struct ReadConfigResult {
  ReadConfigResult();
  ReadConfigResult(ReadConfigResult&&);
  ReadConfigResult& operator=(ReadConfigResult&&);
  ~ReadConfigResult();

  ReadConfigStatus status = ReadConfigStatus::kNotFound;
  MachinesConfig config;
};

// Parses machines.json: {"allowed_cidrs": [...], "machines": [...]}. Comments
// and trailing commas are allowed. nullopt if it is not such an object.
std::optional<MachinesConfig> ParseMachinesConfig(std::string_view json);

// Reads `data_dir`/machines.json. Blocking.
ReadConfigResult ReadMachinesConfig(const base::FilePath& data_dir);

}  // namespace ko_machines

#endif  // BRAVE_COMPONENTS_KO_MACHINES_CORE_MACHINES_CONFIG_H_
