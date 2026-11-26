// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef BRAVE_COMPONENTS_KO_MACHINES_CORE_PREF_NAMES_H_
#define BRAVE_COMPONENTS_KO_MACHINES_CORE_PREF_NAMES_H_

namespace ko_machines::prefs {

// Profile prefs. The machine list cannot live at "ko.machines" itself: prefs
// are stored as a nested dictionary, so "ko.machines" must be a dict for
// "ko.machines.allowed_cidrs" to exist next to it.

// List of {id, name, host, port, kind} dicts.
inline constexpr char kMachines[] = "ko.machines.list";
// List of CIDR strings the bridge may connect to.
inline constexpr char kAllowedCidrs[] = "ko.machines.allowed_cidrs";
// Dict of machine id -> base64 OSCrypt ciphertext of the credentials.
inline constexpr char kCredentials[] = "ko.machines.credentials";
// Whether machines.json in the K-O data dir was read on first use.
inline constexpr char kSeeded[] = "ko.machines.seeded";

}  // namespace ko_machines::prefs

#endif  // BRAVE_COMPONENTS_KO_MACHINES_CORE_PREF_NAMES_H_
