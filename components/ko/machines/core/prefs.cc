// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/ko/machines/core/prefs.h"

#include <set>
#include <utility>

#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "brave/components/ko/machines/core/pref_names.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"

namespace ko_machines {

namespace {

std::string HostPortKey(const Machine& machine) {
  return base::StrCat({machine.host, ":", base::NumberToString(machine.port)});
}

}  // namespace

void RegisterProfilePrefs(PrefRegistrySimple* registry) {
  registry->RegisterListPref(prefs::kMachines);
  registry->RegisterListPref(prefs::kAllowedCidrs,
                             base::ListValue().Append(kDefaultAllowedCidr));
  registry->RegisterDictionaryPref(prefs::kCredentials);
  registry->RegisterBooleanPref(prefs::kSeeded, false);
}

std::vector<Machine> GetMachines(const PrefService& prefs) {
  return MachinesFromList(prefs.GetList(prefs::kMachines));
}

void SetMachines(PrefService& prefs, const std::vector<Machine>& machines) {
  prefs.SetList(prefs::kMachines, MachinesToList(machines));
}

std::optional<Machine> FindMachine(const PrefService& prefs,
                                   std::string_view id) {
  for (Machine& machine : GetMachines(prefs)) {
    if (machine.id == id) {
      return std::move(machine);
    }
  }
  return std::nullopt;
}

std::vector<std::string> GetAllowedCidrs(const PrefService& prefs) {
  std::vector<std::string> cidrs;
  for (const base::Value& value : prefs.GetList(prefs::kAllowedCidrs)) {
    if (const std::string* cidr = value.GetIfString()) {
      cidrs.push_back(*cidr);
    }
  }
  return cidrs;
}

void SetAllowedCidrs(PrefService& prefs,
                     const std::vector<std::string>& cidrs) {
  base::ListValue list;
  for (const std::string& cidr : cidrs) {
    list.Append(cidr);
  }
  prefs.SetList(prefs::kAllowedCidrs, std::move(list));
}

AddressAllowlist GetAllowlist(const PrefService& prefs,
                              std::vector<std::string>* rejected) {
  return AddressAllowlist::FromCidrs(GetAllowedCidrs(prefs), rejected);
}

size_t MergeMachines(std::vector<Machine>& machines,
                     const std::vector<Machine>& imported) {
  std::set<std::string> ids;
  std::set<std::string> endpoints;
  for (const Machine& machine : machines) {
    ids.insert(machine.id);
    endpoints.insert(HostPortKey(machine));
  }
  size_t added = 0;
  for (const Machine& machine : imported) {
    if (machines.size() >= kMaxMachines) {
      break;
    }
    if (ids.contains(machine.id) || endpoints.contains(HostPortKey(machine))) {
      continue;
    }
    ids.insert(machine.id);
    endpoints.insert(HostPortKey(machine));
    machines.push_back(machine);
    ++added;
  }
  return added;
}

}  // namespace ko_machines
