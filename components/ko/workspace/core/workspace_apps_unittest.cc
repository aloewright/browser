// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/ko/workspace/core/workspace_apps.h"

#include <string>

#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "components/prefs/testing_pref_service.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ko_workspace {

TEST(KoWorkspaceAppsTest, Validates) {
  auto app = ValidateWorkspaceApp("  Notes ", "https://notes.example.com/");
  ASSERT_TRUE(app);
  EXPECT_EQ(app->name, "Notes");
  EXPECT_EQ(app->url, GURL("https://notes.example.com/"));

  EXPECT_FALSE(ValidateWorkspaceApp("", "https://example.com"));
  EXPECT_FALSE(ValidateWorkspaceApp("   ", "https://example.com"));
  EXPECT_FALSE(ValidateWorkspaceApp("a\nb", "https://example.com"));
  EXPECT_FALSE(ValidateWorkspaceApp(std::string(65, 'a'), "https://x.com"));
  EXPECT_FALSE(ValidateWorkspaceApp("App", "http://example.com"));
  EXPECT_FALSE(ValidateWorkspaceApp("App", "javascript:alert(1)"));
  EXPECT_FALSE(ValidateWorkspaceApp("App", "chrome://settings"));
  EXPECT_FALSE(ValidateWorkspaceApp("App", "https://u:p@example.com"));
  EXPECT_FALSE(ValidateWorkspaceApp("App", "not a url"));
}

TEST(KoWorkspaceAppsTest, ParsesBothFileShapes) {
  const char kArray[] = R"([
    {"name": "Notes", "url": "https://notes.example.com"},
    {"name": "Bad", "url": "http://plain.example.com"},
    {"name": "Dup", "url": "https://notes.example.com"},
    {"url": "https://noname.example.com"},
    42
  ])";
  auto apps = ParseWorkspaceAppsJson(kArray);
  ASSERT_TRUE(apps);
  ASSERT_EQ(apps->size(), 1u);
  EXPECT_EQ((*apps)[0].name, "Notes");

  auto wrapped = ParseWorkspaceAppsJson(
      R"({"apps": [{"name": "Board", "url": "https://board.example.com"}]})");
  ASSERT_TRUE(wrapped);
  ASSERT_EQ(wrapped->size(), 1u);
  EXPECT_EQ((*wrapped)[0].url, GURL("https://board.example.com"));

  EXPECT_FALSE(ParseWorkspaceAppsJson("not json"));
  EXPECT_FALSE(ParseWorkspaceAppsJson(R"({"other": []})"));
}

TEST(KoWorkspaceAppsTest, CapsCount) {
  base::ListValue list;
  for (size_t i = 0; i < kMaxWorkspaceApps + 10; ++i) {
    list.Append(base::DictValue()
                    .Set("name", "App")
                    .Set("url", base::StrCat({"https://a", base::NumberToString(i),
                                              ".example.com/"})));
  }
  EXPECT_EQ(WorkspaceAppsFromList(list).size(), kMaxWorkspaceApps);
}

TEST(KoWorkspaceAppsTest, PrefsRoundTrip) {
  TestingPrefServiceSimple prefs;
  RegisterWorkspaceProfilePrefs(prefs.registry());
  EXPECT_TRUE(GetWorkspaceApps(prefs).empty());
  EXPECT_FALSE(prefs.GetBoolean(kWorkspaceAppsSeededPref));

  const std::vector<WorkspaceApp> apps = {
      {"Notes", GURL("https://notes.example.com/")},
      {"Board", GURL("https://board.example.com/")},
  };
  SetWorkspaceApps(prefs, apps);
  EXPECT_EQ(GetWorkspaceApps(prefs), apps);
}

}  // namespace ko_workspace
