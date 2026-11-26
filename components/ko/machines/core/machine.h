// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef BRAVE_COMPONENTS_KO_MACHINES_CORE_MACHINE_H_
#define BRAVE_COMPONENTS_KO_MACHINES_CORE_MACHINE_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/values.h"

namespace ko_machines {

enum class MachineKind {
  // macOS Screen Sharing: Apple Remote Desktop auth (RFB security type 30)
  // with a macOS account, or VNC password if enabled on the Mac.
  kScreenSharing,
  // Any RFB server with VNC password auth (or none).
  kVnc,
};

inline constexpr char kKindScreenSharing[] = "macos-screen-sharing";
inline constexpr char kKindVnc[] = "vnc";

inline constexpr uint16_t kDefaultPort = 5900;
inline constexpr size_t kMaxIdLength = 64;
inline constexpr size_t kMaxNameLength = 128;
inline constexpr size_t kMaxMachines = 64;

struct Machine {
  Machine();
  Machine(const Machine&);
  Machine& operator=(const Machine&);
  Machine(Machine&&);
  Machine& operator=(Machine&&);
  ~Machine();

  bool operator==(const Machine&) const = default;

  // Serialized as {id, name, host, port, kind}.
  base::DictValue ToValue() const;
  // Validates every field; nullopt if any is invalid. A missing id is
  // generated, a missing port defaults to 5900 and a missing kind to
  // macos-screen-sharing.
  static std::optional<Machine> FromValue(const base::DictValue& value);

  std::string id;
  std::string name;
  // Canonical hostname or IP literal (IPv6 in brackets).
  std::string host;
  uint16_t port = kDefaultPort;
  MachineKind kind = MachineKind::kScreenSharing;
};

std::string_view MachineKindToString(MachineKind kind);
std::optional<MachineKind> MachineKindFromString(std::string_view kind);

// ASCII letters, digits, '-' and '_', 1 to kMaxIdLength characters.
bool IsValidMachineId(std::string_view id);
std::string GenerateMachineId();

// Trimmed, valid UTF-8 without control characters, 1 to kMaxNameLength bytes.
std::optional<std::string> NormalizeMachineName(std::string_view name);

// A bare hostname or IP literal (no scheme, port, path or credentials),
// canonicalized. IPv6 literals may be given with or without brackets.
std::optional<std::string> CanonicalizeMachineHost(std::string_view host);

// Ports 1-65535 are valid.
std::optional<uint16_t> ValidateMachinePort(int port);

// Parses a list of machine dicts, skipping invalid entries and duplicate ids.
// `skipped`, if set, receives the number of skipped entries.
std::vector<Machine> MachinesFromList(const base::ListValue& list,
                                      size_t* skipped = nullptr);
base::ListValue MachinesToList(const std::vector<Machine>& machines);

}  // namespace ko_machines

#endif  // BRAVE_COMPONENTS_KO_MACHINES_CORE_MACHINE_H_
