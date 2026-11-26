// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef BRAVE_BROWSER_UI_WEBUI_KO_WORKSPACE_KO_WORKSPACE_UI_H_
#define BRAVE_BROWSER_UI_WEBUI_KO_WORKSPACE_KO_WORKSPACE_UI_H_

#include <memory>

#include "brave/components/ko/workspace/common/ko_workspace.mojom.h"
#include "content/public/browser/webui_config.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "ui/webui/mojo_web_ui_controller.h"

namespace ko_workspace {
class WorkspaceHandler;
}  // namespace ko_workspace

inline constexpr char kKoWorkspaceHost[] = "ko-workspace";
inline constexpr char kKoWorkspaceURL[] = "chrome://ko-workspace/";

// chrome://ko-workspace: K-O multi-agent workspaces, backed by ko-agentd.
class KoWorkspaceUI : public ui::MojoWebUIController {
 public:
  explicit KoWorkspaceUI(content::WebUI* web_ui);
  ~KoWorkspaceUI() override;

  KoWorkspaceUI(const KoWorkspaceUI&) = delete;
  KoWorkspaceUI& operator=(const KoWorkspaceUI&) = delete;

  void BindInterface(
      mojo::PendingReceiver<ko_workspace::mojom::PageHandler> receiver);

 private:
  std::unique_ptr<ko_workspace::WorkspaceHandler> handler_;

  WEB_UI_CONTROLLER_TYPE_DECL();
};

class KoWorkspaceUIConfig
    : public content::DefaultWebUIConfig<KoWorkspaceUI> {
 public:
  KoWorkspaceUIConfig();

  // content::WebUIConfig:
  bool IsWebUIEnabled(content::BrowserContext* browser_context) override;
};

#endif  // BRAVE_BROWSER_UI_WEBUI_KO_WORKSPACE_KO_WORKSPACE_UI_H_
