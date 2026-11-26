// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/ko/workspace/core/workspace_apps.h"

#include <algorithm>
#include <set>
#include <utility>

#include "base/json/json_reader.h"
#include "base/strings/string_util.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"
#include "url/url_constants.h"

namespace ko_workspace {

namespace {

constexpr char kNameKey[] = "name";
constexpr char kUrlKey[] = "url";
constexpr char kAppsKey[] = "apps";
constexpr size_t kMaxUrlLength = 2048;

}  // namespace

std::optional<WorkspaceApp> ValidateWorkspaceApp(std::string_view name,
                                                 std::string_view url) {
  const std::string_view trimmed_name =
      base::TrimWhitespaceASCII(name, base::TRIM_ALL);
  if (trimmed_name.empty() ||
      trimmed_name.size() > kMaxWorkspaceAppNameLength ||
      !base::IsStringUTF8(trimmed_name) ||
      std::ranges::any_of(trimmed_name,
                          [](char c) { return base::IsAsciiControl(c); })) {
    return std::nullopt;
  }
  if (url.size() > kMaxUrlLength) {
    return std::nullopt;
  }
  GURL gurl(url);
  if (!gurl.is_valid() || !gurl.SchemeIs(url::kHttpsScheme) ||
      gurl.host().empty() || gurl.has_username() || gurl.has_password()) {
    return std::nullopt;
  }
  return WorkspaceApp{std::string(trimmed_name), std::move(gurl)};
}

std::vector<WorkspaceApp> WorkspaceAppsFromList(const base::ListValue& list) {
  std::vector<WorkspaceApp> apps;
  std::set<GURL> seen;
  for (const base::Value& item : list) {
    if (apps.size() >= kMaxWorkspaceApps) {
      break;
    }
    const base::DictValue* dict = item.GetIfDict();
    if (!dict) {
      continue;
    }
    const std::string* name = dict->FindString(kNameKey);
    const std::string* url = dict->FindString(kUrlKey);
    if (!name || !url) {
      continue;
    }
    auto app = ValidateWorkspaceApp(*name, *url);
    if (!app || !seen.insert(app->url).second) {
      continue;
    }
    apps.push_back(std::move(*app));
  }
  return apps;
}

base::ListValue WorkspaceAppsToList(const std::vector<WorkspaceApp>& apps) {
  base::ListValue list;
  for (const auto& app : apps) {
    list.Append(
        base::DictValue().Set(kNameKey, app.name).Set(kUrlKey, app.url.spec()));
  }
  return list;
}

std::optional<std::vector<WorkspaceApp>> ParseWorkspaceAppsJson(
    std::string_view json) {
  auto value = base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  if (!value) {
    return std::nullopt;
  }
  if (const base::ListValue* list = value->GetIfList()) {
    return WorkspaceAppsFromList(*list);
  }
  if (const base::DictValue* dict = value->GetIfDict()) {
    if (const base::ListValue* list = dict->FindList(kAppsKey)) {
      return WorkspaceAppsFromList(*list);
    }
  }
  return std::nullopt;
}

void RegisterWorkspaceProfilePrefs(PrefRegistrySimple* registry) {
  registry->RegisterListPref(kWorkspaceAppsPref);
  registry->RegisterBooleanPref(kWorkspaceAppsSeededPref, false);
}

std::vector<WorkspaceApp> GetWorkspaceApps(const PrefService& prefs) {
  return WorkspaceAppsFromList(prefs.GetList(kWorkspaceAppsPref));
}

void SetWorkspaceApps(PrefService& prefs,
                      const std::vector<WorkspaceApp>& apps) {
  prefs.SetList(kWorkspaceAppsPref, WorkspaceAppsToList(apps));
}

}  // namespace ko_workspace
