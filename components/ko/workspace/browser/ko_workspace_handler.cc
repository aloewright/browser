// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/components/ko/workspace/browser/ko_workspace_handler.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "brave/components/ko/workspace/core/agentd_config.h"
#include "components/prefs/pref_service.h"
#include "url/url_constants.h"

namespace ko_workspace {

namespace {

// Matches the Brave Containers storage partition key charset.
bool IsValidContainerId(std::string_view id) {
  return !id.empty() && id.size() <= 64 && std::ranges::all_of(id, [](char c) {
           return base::IsAsciiAlphaNumeric(c) || c == '-';
         });
}

std::optional<std::vector<WorkspaceApp>> ReadAppsFile(
    const base::FilePath& data_dir) {
  if (data_dir.empty()) {
    return std::nullopt;
  }
  // Not secret, but must not be writable by others.
  auto contents =
      ReadUserFile(data_dir.AppendASCII(kWorkspaceAppsFileName),
                   kMaxWorkspaceAppsFileSize,
                   /*allow_group_or_world_read=*/true, nullptr);
  if (!contents) {
    return std::nullopt;
  }
  return ParseWorkspaceAppsJson(*contents);
}

}  // namespace

WorkspaceHandler::WorkspaceHandler(
    mojo::PendingReceiver<mojom::PageHandler> receiver,
    PrefService& prefs,
    base::FilePath data_dir,
    std::unique_ptr<Delegate> delegate)
    : receiver_(this, std::move(receiver)),
      prefs_(prefs),
      data_dir_(std::move(data_dir)),
      delegate_(std::move(delegate)) {
  CHECK(delegate_);
}

WorkspaceHandler::~WorkspaceHandler() = default;

void WorkspaceHandler::GetContainers(GetContainersCallback callback) {
  std::move(callback).Run(delegate_->GetContainers());
}

void WorkspaceHandler::CreateContainer(const std::string& name,
                                       CreateContainerCallback callback) {
  const std::string_view trimmed =
      base::TrimWhitespaceASCII(name, base::TRIM_ALL);
  if (trimmed.empty() || trimmed.size() > kMaxContainerNameLength ||
      !base::IsStringUTF8(trimmed)) {
    std::move(callback).Run(nullptr);
    return;
  }
  std::move(callback).Run(delegate_->CreateContainer(std::string(trimmed)));
}

void WorkspaceHandler::OpenTabsInContainer(
    const std::string& container_id,
    const std::vector<GURL>& urls,
    OpenTabsInContainerCallback callback) {
  if (!IsValidContainerId(container_id) ||
      !delegate_->ContainerExists(container_id)) {
    std::move(callback).Run(0);
    return;
  }
  uint32_t opened = 0;
  for (const GURL& url : urls) {
    if (opened >= kMaxTabsPerOpen) {
      break;
    }
    if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS()) {
      continue;
    }
    delegate_->OpenUrlInContainer(container_id, url);
    ++opened;
  }
  std::move(callback).Run(opened);
}

void WorkspaceHandler::GetWorkspaceApps(GetWorkspaceAppsCallback callback) {
  if (prefs_->GetBoolean(kWorkspaceAppsSeededPref)) {
    std::move(callback).Run(GetAppsForPage());
    return;
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&ReadAppsFile, data_dir_),
      base::BindOnce(
          &WorkspaceHandler::OnAppsFileRead, weak_factory_.GetWeakPtr(),
          /*replace_existing=*/false,
          base::BindOnce(
              [](base::WeakPtr<WorkspaceHandler> handler,
                 GetWorkspaceAppsCallback callback, bool) {
                std::move(callback).Run(
                    handler ? handler->GetAppsForPage()
                            : std::vector<mojom::WorkspaceAppPtr>());
              },
              weak_factory_.GetWeakPtr(), std::move(callback))));
}

void WorkspaceHandler::ReloadWorkspaceAppsFromFile(
    ReloadWorkspaceAppsFromFileCallback callback) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&ReadAppsFile, data_dir_),
      base::BindOnce(
          &WorkspaceHandler::OnAppsFileRead, weak_factory_.GetWeakPtr(),
          /*replace_existing=*/true,
          base::BindOnce(
              [](base::WeakPtr<WorkspaceHandler> handler,
                 ReloadWorkspaceAppsFromFileCallback callback, bool success) {
                std::move(callback).Run(
                    success, handler ? handler->GetAppsForPage()
                                     : std::vector<mojom::WorkspaceAppPtr>());
              },
              weak_factory_.GetWeakPtr(), std::move(callback))));
}

void WorkspaceHandler::OpenWorkspaceApp(const GURL& url, bool in_side_panel) {
  // Only URLs from the user's own list.
  const auto apps = ko_workspace::GetWorkspaceApps(*prefs_);
  if (std::ranges::none_of(apps, [&url](const WorkspaceApp& app) {
        return app.url == url;
      })) {
    return;
  }
  delegate_->OpenWorkspaceApp(url, in_side_panel);
}

void WorkspaceHandler::CaptureActiveTab(mojom::CaptureKind kind,
                                        CaptureActiveTabCallback callback) {
  delegate_->CaptureActiveTab(kind, std::move(callback));
}

void WorkspaceHandler::ShowWorkspace(bool in_side_panel) {
  delegate_->ShowWorkspace(in_side_panel);
}

void WorkspaceHandler::OnAppsFileRead(
    bool replace_existing,
    base::OnceCallback<void(bool)> done,
    std::optional<std::vector<WorkspaceApp>> apps) {
  if (apps) {
    if (replace_existing || !prefs_->GetBoolean(kWorkspaceAppsSeededPref)) {
      SetWorkspaceApps(*prefs_, *apps);
    }
    prefs_->SetBoolean(kWorkspaceAppsSeededPref, true);
  }
  std::move(done).Run(apps.has_value());
}

std::vector<mojom::WorkspaceAppPtr> WorkspaceHandler::GetAppsForPage() const {
  std::vector<mojom::WorkspaceAppPtr> result;
  for (const auto& app : ko_workspace::GetWorkspaceApps(*prefs_)) {
    result.push_back(mojom::WorkspaceApp::New(app.name, app.url));
  }
  return result;
}

}  // namespace ko_workspace
