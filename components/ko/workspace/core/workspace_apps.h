// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef BRAVE_COMPONENTS_KO_WORKSPACE_CORE_WORKSPACE_APPS_H_
#define BRAVE_COMPONENTS_KO_WORKSPACE_CORE_WORKSPACE_APPS_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/values.h"
#include "url/gurl.h"

class PrefRegistrySimple;
class PrefService;

namespace ko_workspace {

// Profile prefs.
inline constexpr char kWorkspaceAppsPref[] = "ko.workspace.apps";
inline constexpr char kWorkspaceAppsSeededPref[] = "ko.workspace.apps_seeded";

inline constexpr size_t kMaxWorkspaceApps = 50;
inline constexpr size_t kMaxWorkspaceAppNameLength = 64;
inline constexpr size_t kMaxWorkspaceAppsFileSize = 64 * 1024;

// A user's web app (e.g. behind Cloudflare Access) opened in a tab or the
// side panel. Only https URLs without embedded credentials are accepted.
struct WorkspaceApp {
  std::string name;
  GURL url;

  bool operator==(const WorkspaceApp&) const = default;
};

std::optional<WorkspaceApp> ValidateWorkspaceApp(std::string_view name,
                                                 std::string_view url);

// Drops invalid entries and duplicate URLs; keeps at most kMaxWorkspaceApps.
std::vector<WorkspaceApp> WorkspaceAppsFromList(const base::ListValue& list);
base::ListValue WorkspaceAppsToList(const std::vector<WorkspaceApp>& apps);

// workspace_apps.json: either [{"name", "url"}] or {"apps": [...]}.
std::optional<std::vector<WorkspaceApp>> ParseWorkspaceAppsJson(
    std::string_view json);

void RegisterWorkspaceProfilePrefs(PrefRegistrySimple* registry);
std::vector<WorkspaceApp> GetWorkspaceApps(const PrefService& prefs);
void SetWorkspaceApps(PrefService& prefs,
                      const std::vector<WorkspaceApp>& apps);

}  // namespace ko_workspace

#endif  // BRAVE_COMPONENTS_KO_WORKSPACE_CORE_WORKSPACE_APPS_H_
