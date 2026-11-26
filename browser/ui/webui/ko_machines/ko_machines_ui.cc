// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/browser/ui/webui/ko_machines/ko_machines_ui.h"

#include <utility>

#include "base/functional/bind.h"
#include "brave/components/ko/machines/browser/machine_bridge.h"
#include "brave/components/ko/machines/browser/machines_handler.h"
#include "brave/components/ko/machines/core/machines_config.h"
#include "brave/components/ko/machines/resources/grit/ko_machines_generated_map.h"
#include "brave/components/ko/machines/resources/grit/ko_machines_static_resources.h"
#include "brave/components/ko/machines/resources/grit/ko_machines_static_resources_map.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "components/grit/brave_components_webui_strings.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/url_constants.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"
#include "ui/webui/webui_util.h"

namespace {

network::mojom::NetworkContext* GetNetworkContext(Profile* profile) {
  return profile->GetDefaultStoragePartition()->GetNetworkContext();
}

void CreateAndAddDataSource(Profile* profile) {
  content::WebUIDataSource* source =
      content::WebUIDataSource::CreateAndAdd(profile, kKOMachinesHost);
  // Unknown paths (e.g. /view) get the page; it picks the view itself.
  webui::SetupWebUIDataSource(source, kKoMachinesGenerated,
                              IDR_KO_MACHINES_STATIC_KO_MACHINES_HTML);
  source->AddResourcePaths(kKoMachinesStaticResources);
  source->AddLocalizedStrings(webui::kKOMachinesStrings);

  // Leo icons come from chrome://resources; noVNC draws the remote cursor and
  // TightPNG rectangles from data: URLs.
  source->OverrideContentSecurityPolicy(
      network::mojom::CSPDirectiveName::ImgSrc,
      "img-src chrome://resources 'self' data:;");
  // The page has no network access of its own: RFB goes through Mojo.
  source->OverrideContentSecurityPolicy(
      network::mojom::CSPDirectiveName::ConnectSrc,
      "connect-src chrome://resources 'self';");
  source->OverrideContentSecurityPolicy(
      network::mojom::CSPDirectiveName::FrameSrc, "frame-src 'none';");
}

}  // namespace

KOMachinesUIConfig::KOMachinesUIConfig()
    : DefaultWebUIConfig(content::kChromeUIScheme, kKOMachinesHost) {}

bool KOMachinesUIConfig::IsWebUIEnabled(
    content::BrowserContext* browser_context) {
  // Machines and their credentials belong to a regular profile.
  return !browser_context->IsOffTheRecord();
}

KOMachinesUI::KOMachinesUI(content::WebUI* web_ui)
    : ui::MojoWebUIController(web_ui) {
  CreateAndAddDataSource(Profile::FromWebUI(web_ui));
}

KOMachinesUI::~KOMachinesUI() = default;

void KOMachinesUI::BindInterface(
    mojo::PendingReceiver<ko_machines::mojom::MachinesHandler> receiver) {
  Profile* profile = Profile::FromWebUI(web_ui());
  handler_ = std::make_unique<ko_machines::MachinesHandler>(
      std::move(receiver), *profile->GetPrefs(),
      g_browser_process->os_crypt_async(), ko_machines::GetKODataDir());
}

void KOMachinesUI::BindInterface(
    mojo::PendingReceiver<ko_machines::mojom::KOMachineBridge> receiver) {
  Profile* profile = Profile::FromWebUI(web_ui());
  // The profile outlives this WebUI and therefore the bridge.
  bridge_ = std::make_unique<ko_machines::MachineBridge>(
      std::move(receiver), *profile->GetPrefs(),
      base::BindRepeating(&GetNetworkContext, base::Unretained(profile)));
}

WEB_UI_CONTROLLER_TYPE_IMPL(KOMachinesUI)
