// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef BRAVE_BROWSER_UI_WEBUI_KO_MACHINES_KO_MACHINES_UI_H_
#define BRAVE_BROWSER_UI_WEBUI_KO_MACHINES_KO_MACHINES_UI_H_

#include <memory>

#include "brave/components/ko/machines/common/ko_machines.mojom.h"
#include "content/public/browser/webui_config.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "ui/webui/mojo_web_ui_controller.h"

namespace content {
class BrowserContext;
class WebUI;
}  // namespace content

namespace ko_machines {
class MachineBridge;
class MachinesHandler;
}  // namespace ko_machines

inline constexpr char kKOMachinesHost[] = "ko-machines";

class KOMachinesUI;

class KOMachinesUIConfig : public content::DefaultWebUIConfig<KOMachinesUI> {
 public:
  KOMachinesUIConfig();

  // content::WebUIConfig:
  bool IsWebUIEnabled(content::BrowserContext* browser_context) override;
};

// chrome://ko-machines (list) and chrome://ko-machines/view?id=<id> (viewer).
// Its interfaces are registered for this controller type only, so no other
// renderer can bind KOMachineBridge.
class KOMachinesUI : public ui::MojoWebUIController {
 public:
  explicit KOMachinesUI(content::WebUI* web_ui);
  KOMachinesUI(const KOMachinesUI&) = delete;
  KOMachinesUI& operator=(const KOMachinesUI&) = delete;
  ~KOMachinesUI() override;

  void BindInterface(
      mojo::PendingReceiver<ko_machines::mojom::MachinesHandler> receiver);
  void BindInterface(
      mojo::PendingReceiver<ko_machines::mojom::KOMachineBridge> receiver);

 private:
  std::unique_ptr<ko_machines::MachinesHandler> handler_;
  std::unique_ptr<ko_machines::MachineBridge> bridge_;

  WEB_UI_CONTROLLER_TYPE_DECL();
};

#endif  // BRAVE_BROWSER_UI_WEBUI_KO_MACHINES_KO_MACHINES_UI_H_
