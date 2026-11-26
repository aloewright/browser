// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/ko/machines/core/machine.h"

#include <set>
#include <utility>

#include "base/strings/string_util.h"
#include "base/uuid.h"
#include "net/base/url_util.h"
#include "url/url_canon.h"

namespace ko_machines {

namespace {

constexpr char kIdKey[] = "id";
constexpr char kNameKey[] = "name";
constexpr char kHostKey[] = "host";
constexpr char kPortKey[] = "port";
constexpr char kKindKey[] = "kind";

// RFC 1035 limit plus room for IPv6 brackets.
constexpr size_t kMaxHostLength = 255;

}  // namespace

Machine::Machine() = default;
Machine::Machine(const Machine&) = default;
Machine& Machine::operator=(const Machine&) = default;
Machine::Machine(Machine&&) = default;
Machine& Machine::operator=(Machine&&) = default;
Machine::~Machine() = default;

base::DictValue Machine::ToValue() const {
  base::DictValue value;
  value.Set(kIdKey, id);
  value.Set(kNameKey, name);
  value.Set(kHostKey, host);
  value.Set(kPortKey, static_cast<int>(port));
  value.Set(kKindKey, MachineKindToString(kind));
  return value;
}

// static
std::optional<Machine> Machine::FromValue(const base::DictValue& value) {
  Machine machine;

  if (const std::string* id = value.FindString(kIdKey)) {
    if (!IsValidMachineId(*id)) {
      return std::nullopt;
    }
    machine.id = *id;
  } else if (value.Find(kIdKey)) {
    return std::nullopt;
  } else {
    machine.id = GenerateMachineId();
  }

  const std::string* name = value.FindString(kNameKey);
  std::optional<std::string> normalized_name =
      name ? NormalizeMachineName(*name) : std::nullopt;
  if (!normalized_name) {
    return std::nullopt;
  }
  machine.name = std::move(*normalized_name);

  const std::string* host = value.FindString(kHostKey);
  std::optional<std::string> canonical_host =
      host ? CanonicalizeMachineHost(*host) : std::nullopt;
  if (!canonical_host) {
    return std::nullopt;
  }
  machine.host = std::move(*canonical_host);

  if (const base::Value* port = value.Find(kPortKey)) {
    std::optional<uint16_t> valid_port =
        port->is_int() ? ValidateMachinePort(port->GetInt()) : std::nullopt;
    if (!valid_port) {
      return std::nullopt;
    }
    machine.port = *valid_port;
  }

  if (const base::Value* kind = value.Find(kKindKey)) {
    std::optional<MachineKind> valid_kind =
        kind->is_string() ? MachineKindFromString(kind->GetString())
                          : std::nullopt;
    if (!valid_kind) {
      return std::nullopt;
    }
    machine.kind = *valid_kind;
  }

  return machine;
}

std::string_view MachineKindToString(MachineKind kind) {
  switch (kind) {
    case MachineKind::kScreenSharing:
      return kKindScreenSharing;
    case MachineKind::kVnc:
      return kKindVnc;
  }
}

std::optional<MachineKind> MachineKindFromString(std::string_view kind) {
  if (kind == kKindScreenSharing) {
    return MachineKind::kScreenSharing;
  }
  if (kind == kKindVnc) {
    return MachineKind::kVnc;
  }
  return std::nullopt;
}

bool IsValidMachineId(std::string_view id) {
  if (id.empty() || id.size() > kMaxIdLength) {
    return false;
  }
  for (char c : id) {
    if (!base::IsAsciiAlphaNumeric(c) && c != '-' && c != '_') {
      return false;
    }
  }
  return true;
}

std::string GenerateMachineId() {
  return base::Uuid::GenerateRandomV4().AsLowercaseString();
}

std::optional<std::string> NormalizeMachineName(std::string_view name) {
  std::string_view trimmed = base::TrimWhitespaceASCII(name, base::TRIM_ALL);
  if (trimmed.empty() || trimmed.size() > kMaxNameLength ||
      !base::IsStringUTF8(trimmed)) {
    return std::nullopt;
  }
  for (char c : trimmed) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte == 0x7f) {
      return std::nullopt;
    }
  }
  return std::string(trimmed);
}

std::optional<std::string> CanonicalizeMachineHost(std::string_view host) {
  host = base::TrimWhitespaceASCII(host, base::TRIM_ALL);
  if (host.empty() || host.size() > kMaxHostLength) {
    return std::nullopt;
  }
  url::CanonHostInfo info;
  // Bracketed IPv6 goes straight to the URL host parser; anything else with
  // a ':' is tried as bare IPv6 (and fails if it is host:port).
  const std::string canonical =
      host.front() == '[' ? net::CanonicalizeHost(host, &info)
                          : net::CanonicalizeHostSupportsBareIPV6(host, &info);
  if (canonical.empty() || info.family == url::CanonHostInfo::BROKEN) {
    return std::nullopt;
  }
  if (!info.IsIPAddress() && !net::IsCanonicalizedHostCompliant(canonical)) {
    return std::nullopt;
  }
  return canonical;
}

std::optional<uint16_t> ValidateMachinePort(int port) {
  if (port < 1 || port > 65535) {
    return std::nullopt;
  }
  return static_cast<uint16_t>(port);
}

std::vector<Machine> MachinesFromList(const base::ListValue& list,
                                      size_t* skipped) {
  std::vector<Machine> machines;
  std::set<std::string> ids;
  size_t skipped_count = 0;
  for (const base::Value& entry : list) {
    const base::DictValue* dict = entry.GetIfDict();
    std::optional<Machine> machine =
        dict ? Machine::FromValue(*dict) : std::nullopt;
    if (!machine || machines.size() >= kMaxMachines ||
        !ids.insert(machine->id).second) {
      ++skipped_count;
      continue;
    }
    machines.push_back(std::move(*machine));
  }
  if (skipped) {
    *skipped = skipped_count;
  }
  return machines;
}

base::ListValue MachinesToList(const std::vector<Machine>& machines) {
  base::ListValue list;
  for (const Machine& machine : machines) {
    list.Append(machine.ToValue());
  }
  return list;
}

}  // namespace ko_machines
