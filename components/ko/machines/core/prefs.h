// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef BRAVE_COMPONENTS_KO_MACHINES_CORE_PREFS_H_
#define BRAVE_COMPONENTS_KO_MACHINES_CORE_PREFS_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "brave/components/ko/machines/core/address_allowlist.h"
#include "brave/components/ko/machines/core/machine.h"

class PrefRegistrySimple;
class PrefService;

namespace ko_machines {

// The NetBird / CGNAT range.
inline constexpr char kDefaultAllowedCidr[] = "100.64.0.0/10";

void RegisterProfilePrefs(PrefRegistrySimple* registry);

std::vector<Machine> GetMachines(const PrefService& prefs);
void SetMachines(PrefService& prefs, const std::vector<Machine>& machines);
std::optional<Machine> FindMachine(const PrefService& prefs,
                                   std::string_view id);

// The configured CIDR strings, as stored.
std::vector<std::string> GetAllowedCidrs(const PrefService& prefs);
void SetAllowedCidrs(PrefService& prefs, const std::vector<std::string>& cidrs);
// The usable allowlist; unusable entries go to `rejected` if set.
AddressAllowlist GetAllowlist(const PrefService& prefs,
                              std::vector<std::string>* rejected = nullptr);

// Adds `imported` machines whose id and host:port are not already present.
// Returns how many were added.
size_t MergeMachines(std::vector<Machine>& machines,
                     const std::vector<Machine>& imported);

}  // namespace ko_machines

#endif  // BRAVE_COMPONENTS_KO_MACHINES_CORE_PREFS_H_
