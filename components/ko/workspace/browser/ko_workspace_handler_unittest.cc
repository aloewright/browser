// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/ko/workspace/browser/ko_workspace_handler.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/memory/raw_ptr.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "brave/components/ko/workspace/core/agentd_config.h"
#include "brave/components/ko/workspace/core/workspace_apps.h"
#include "build/build_config.h"
#include "components/prefs/testing_pref_service.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace ko_workspace {

namespace {

class FakeDelegate : public WorkspaceHandler::Delegate {
 public:
  std::vector<mojom::WorkspaceContainerPtr> GetContainers() override {
    std::vector<mojom::WorkspaceContainerPtr> result;
    result.push_back(mojom::WorkspaceContainer::New("c-1", "Work"));
    return result;
  }
  mojom::WorkspaceContainerPtr CreateContainer(
      const std::string& name) override {
    created_names.push_back(name);
    return mojom::WorkspaceContainer::New("c-new", name);
  }
  bool ContainerExists(const std::string& id) override { return id == "c-1"; }
  void OpenUrlInContainer(const std::string& container_id,
                          const GURL& url) override {
    opened.emplace_back(container_id, url);
  }
  void OpenWorkspaceApp(const GURL& url, bool in_side_panel) override {
    opened_apps.emplace_back(url, in_side_panel);
  }
  void CaptureActiveTab(
      mojom::CaptureKind kind,
      WorkspaceHandler::CaptureActiveTabCallback callback) override {
    std::move(callback).Run(nullptr);
  }
  void ShowWorkspace(bool in_side_panel) override {}

  std::vector<std::string> created_names;
  std::vector<std::pair<std::string, GURL>> opened;
  std::vector<std::pair<GURL, bool>> opened_apps;
};

}  // namespace

class KoWorkspaceHandlerTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    RegisterWorkspaceProfilePrefs(prefs_.registry());
    auto delegate = std::make_unique<FakeDelegate>();
    delegate_ = delegate.get();
    handler_ = std::make_unique<WorkspaceHandler>(
        remote_.BindNewPipeAndPassReceiver(), prefs_, temp_dir_.GetPath(),
        std::move(delegate));
  }

  void TearDown() override {
    delegate_ = nullptr;
    handler_.reset();
  }

  void WriteAppsFile(std::string_view json) {
    const base::FilePath path =
        temp_dir_.GetPath().AppendASCII(kWorkspaceAppsFileName);
    ASSERT_TRUE(base::WriteFile(path, json));
#if BUILDFLAG(IS_POSIX)
    ASSERT_TRUE(base::SetPosixFilePermissions(path, 0644));
#endif
  }

  std::vector<mojom::WorkspaceAppPtr> GetApps() {
    base::test::TestFuture<std::vector<mojom::WorkspaceAppPtr>> future;
    handler_->GetWorkspaceApps(future.GetCallback());
    return future.Take();
  }

  uint32_t OpenTabs(const std::string& id, std::vector<GURL> urls) {
    base::test::TestFuture<uint32_t> future;
    handler_->OpenTabsInContainer(id, urls, future.GetCallback());
    return future.Get();
  }

  base::test::TaskEnvironment task_environment_;
  base::ScopedTempDir temp_dir_;
  TestingPrefServiceSimple prefs_;
  mojo::Remote<mojom::PageHandler> remote_;
  raw_ptr<FakeDelegate> delegate_ = nullptr;
  std::unique_ptr<WorkspaceHandler> handler_;
};

TEST_F(KoWorkspaceHandlerTest, CreateContainerValidatesName) {
  base::test::TestFuture<mojom::WorkspaceContainerPtr> future;
  handler_->CreateContainer("  Research  ", future.GetCallback());
  auto container = future.Take();
  ASSERT_TRUE(container);
  EXPECT_EQ(container->name, "Research");

  for (const std::string& bad :
       {std::string(), std::string("   "), std::string(65, 'x')}) {
    handler_->CreateContainer(bad, future.GetCallback());
    EXPECT_FALSE(future.Take());
  }
  EXPECT_EQ(delegate_->created_names, std::vector<std::string>{"Research"});
}

TEST_F(KoWorkspaceHandlerTest, OpenTabsOnlyInKnownContainersAndWebUrls) {
  EXPECT_EQ(
      OpenTabs("c-1", {GURL("https://example.com/"),
                       GURL("http://example.org/"), GURL("chrome://settings"),
                       GURL("javascript:alert(1)"), GURL("file:///etc")}),
      2u);
  ASSERT_EQ(delegate_->opened.size(), 2u);
  EXPECT_EQ(delegate_->opened[0].first, "c-1");

  EXPECT_EQ(OpenTabs("unknown", {GURL("https://example.com/")}), 0u);
  EXPECT_EQ(OpenTabs("../c-1", {GURL("https://example.com/")}), 0u);
  EXPECT_EQ(OpenTabs("", {GURL("https://example.com/")}), 0u);

  std::vector<GURL> many(kMaxTabsPerOpen + 5, GURL("https://example.com/"));
  EXPECT_EQ(OpenTabs("c-1", many), kMaxTabsPerOpen);
}

TEST_F(KoWorkspaceHandlerTest, SeedsAppsFromFileOnce) {
  EXPECT_TRUE(GetApps().empty());
  EXPECT_FALSE(prefs_.GetBoolean(kWorkspaceAppsSeededPref));

  WriteAppsFile(R"([{"name": "Notes", "url": "https://notes.example.com/"}])");
  auto apps = GetApps();
  ASSERT_EQ(apps.size(), 1u);
  EXPECT_EQ(apps[0]->name, "Notes");
  EXPECT_TRUE(prefs_.GetBoolean(kWorkspaceAppsSeededPref));

  // Later file edits need an explicit reload.
  WriteAppsFile(R"([{"name": "Board", "url": "https://board.example.com/"}])");
  EXPECT_EQ(GetApps()[0]->name, "Notes");

  base::test::TestFuture<bool, std::vector<mojom::WorkspaceAppPtr>> reload;
  handler_->ReloadWorkspaceAppsFromFile(reload.GetCallback());
  EXPECT_TRUE(reload.Get<0>());
  ASSERT_EQ(reload.Get<1>().size(), 1u);
  EXPECT_EQ(reload.Get<1>()[0]->name, "Board");
}

#if BUILDFLAG(IS_POSIX)
TEST_F(KoWorkspaceHandlerTest, IgnoresWritableAppsFile) {
  WriteAppsFile(R"([{"name": "Notes", "url": "https://notes.example.com/"}])");
  ASSERT_TRUE(base::SetPosixFilePermissions(
      temp_dir_.GetPath().AppendASCII(kWorkspaceAppsFileName), 0666));
  EXPECT_TRUE(GetApps().empty());
}
#endif

TEST_F(KoWorkspaceHandlerTest, OpensOnlyListedApps) {
  SetWorkspaceApps(prefs_, {{"Notes", GURL("https://notes.example.com/")}});
  handler_->OpenWorkspaceApp(GURL("https://notes.example.com/"), true);
  handler_->OpenWorkspaceApp(GURL("https://evil.example.com/"), false);
  ASSERT_EQ(delegate_->opened_apps.size(), 1u);
  EXPECT_EQ(delegate_->opened_apps[0].first,
            GURL("https://notes.example.com/"));
  EXPECT_TRUE(delegate_->opened_apps[0].second);
}

}  // namespace ko_workspace
