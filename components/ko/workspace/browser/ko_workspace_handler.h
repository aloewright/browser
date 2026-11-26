// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef BRAVE_COMPONENTS_KO_WORKSPACE_BROWSER_KO_WORKSPACE_HANDLER_H_
#define BRAVE_COMPONENTS_KO_WORKSPACE_BROWSER_KO_WORKSPACE_HANDLER_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "brave/components/ko/workspace/common/ko_workspace.mojom.h"
#include "brave/components/ko/workspace/core/workspace_apps.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "url/gurl.h"

class PrefService;

namespace ko_workspace {

inline constexpr size_t kMaxTabsPerOpen = 20;
inline constexpr size_t kMaxContainerNameLength = 64;

// Browser side of chrome://ko-workspace. Validates everything the page sends;
// browser operations go through Delegate.
class WorkspaceHandler : public mojom::PageHandler {
 public:
  class Delegate {
   public:
    virtual ~Delegate() = default;

    virtual std::vector<mojom::WorkspaceContainerPtr> GetContainers() = 0;
    virtual mojom::WorkspaceContainerPtr CreateContainer(
        const std::string& name) = 0;
    virtual bool ContainerExists(const std::string& id) = 0;
    virtual void OpenUrlInContainer(const std::string& container_id,
                                    const GURL& url) = 0;
    virtual void OpenWorkspaceApp(const GURL& url, bool in_side_panel) = 0;
    virtual void CaptureActiveTab(mojom::CaptureKind kind,
                                  CaptureActiveTabCallback callback) = 0;
    virtual void ShowWorkspace(bool in_side_panel) = 0;
  };

  WorkspaceHandler(mojo::PendingReceiver<mojom::PageHandler> receiver,
                   PrefService& prefs,
                   base::FilePath data_dir,
                   std::unique_ptr<Delegate> delegate);
  ~WorkspaceHandler() override;

  WorkspaceHandler(const WorkspaceHandler&) = delete;
  WorkspaceHandler& operator=(const WorkspaceHandler&) = delete;

  // mojom::PageHandler:
  void GetContainers(GetContainersCallback callback) override;
  void CreateContainer(const std::string& name,
                       CreateContainerCallback callback) override;
  void OpenTabsInContainer(const std::string& container_id,
                           const std::vector<GURL>& urls,
                           OpenTabsInContainerCallback callback) override;
  void GetWorkspaceApps(GetWorkspaceAppsCallback callback) override;
  void ReloadWorkspaceAppsFromFile(
      ReloadWorkspaceAppsFromFileCallback callback) override;
  void OpenWorkspaceApp(const GURL& url, bool in_side_panel) override;
  void CaptureActiveTab(mojom::CaptureKind kind,
                        CaptureActiveTabCallback callback) override;
  void ShowWorkspace(bool in_side_panel) override;

 private:
  void OnAppsFileRead(bool replace_existing,
                      base::OnceCallback<void(bool)> done,
                      std::optional<std::vector<WorkspaceApp>> apps);
  std::vector<mojom::WorkspaceAppPtr> GetAppsForPage() const;

  mojo::Receiver<mojom::PageHandler> receiver_;
  raw_ref<PrefService> prefs_;
  const base::FilePath data_dir_;
  std::unique_ptr<Delegate> delegate_;
  base::WeakPtrFactory<WorkspaceHandler> weak_factory_{this};
};

}  // namespace ko_workspace

#endif  // BRAVE_COMPONENTS_KO_WORKSPACE_BROWSER_KO_WORKSPACE_HANDLER_H_
