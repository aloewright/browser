/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef BRAVE_COMPONENTS_KO_COMMON_KO_PATHS_H_
#define BRAVE_COMPONENTS_KO_COMMON_KO_PATHS_H_

#include "base/files/file_path.h"

namespace ko {

// The data dir shared with ko-agentd (agentd.token, agentd.port,
// machines.json). macOS: ~/Library/Application Support/K-O. Linux:
// $XDG_DATA_HOME/k-o, else ~/.local/share/k-o. Empty when unknown or on other
// platforms.
base::FilePath GetDataDir();

}  // namespace ko

#endif  // BRAVE_COMPONENTS_KO_COMMON_KO_PATHS_H_
