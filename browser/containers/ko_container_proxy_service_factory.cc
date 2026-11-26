// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/browser/containers/ko_container_proxy_service_factory.h"

#include <optional>
#include <string_view>

#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "brave/browser/containers/containers_service_factory.h"
#include "brave/browser/containers/ko_container_proxy_service.h"
#include "brave/components/containers/content/browser/storage_partition_utils.h"
#include "brave/components/containers/core/browser/containers_service.h"
#include "brave/components/containers/core/common/features.h"
#include "brave/components/containers/core/mojom/containers.mojom.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/net/profile_network_context_service.h"
#include "chrome/browser/net/profile_network_context_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_selections.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/tabs/public/tab_interface.h"

namespace {

std::optional<bool> IsProfileProxyConfigured(Profile* profile) {
  auto* service = ProfileNetworkContextServiceFactory::GetForContext(profile);
  return service ? service->IsProxyConfigured() : std::nullopt;
}

bool ContainerExists(Profile* profile, std::string_view id) {
  auto* service = ContainersServiceFactory::GetForProfile(profile);
  return service && !!service->GetRuntimeContainerById(id);
}

void ForEachContainerTab(
    Profile* profile,
    std::string_view id,
    base::RepeatingCallback<void(content::WebContents*)> visit) {
  for (auto* browser : GetAllBrowserWindowInterfaces()) {
    if (browser->GetProfile() != profile) {
      continue;
    }
    auto* tab_strip = browser->GetTabStripModel();
    if (!tab_strip) {
      continue;
    }
    for (auto* tab : *tab_strip) {
      content::WebContents* contents = tab ? tab->GetContents() : nullptr;
      if (contents && containers::GetContainerIdForWebContents(contents) == id) {
        visit.Run(contents);
      }
    }
  }
}

}  // namespace

// static
KoContainerProxyServiceFactory* KoContainerProxyServiceFactory::GetInstance() {
  static base::NoDestructor<KoContainerProxyServiceFactory> instance;
  return instance.get();
}

// static
containers::KoContainerProxyService*
KoContainerProxyServiceFactory::GetForProfile(Profile* profile) {
  return static_cast<containers::KoContainerProxyService*>(
      GetInstance()->GetServiceForBrowserContext(profile, true));
}

KoContainerProxyServiceFactory::KoContainerProxyServiceFactory()
    : ProfileKeyedServiceFactory(
          "KoContainerProxyService",
          ProfileSelections::Builder()
              .WithRegular(ProfileSelection::kOwnInstance)
              .Build()) {
  DependsOn(ContainersServiceFactory::GetInstance());
  DependsOn(ProfileNetworkContextServiceFactory::GetInstance());
}

KoContainerProxyServiceFactory::~KoContainerProxyServiceFactory() = default;

std::unique_ptr<KeyedService>
KoContainerProxyServiceFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  if (!base::FeatureList::IsEnabled(containers::features::kContainers)) {
    return nullptr;
  }
  auto* profile = Profile::FromBrowserContext(context);
  auto service = std::make_unique<containers::KoContainerProxyService>(
      profile->GetPrefs(), g_browser_process->os_crypt_async(),
      base::BindRepeating(&IsProfileProxyConfigured, base::Unretained(profile)),
      base::BindRepeating(&ContainerExists, base::Unretained(profile)),
      base::BindRepeating(&ForEachContainerTab, base::Unretained(profile)));
  if (!profile->IsOffTheRecord()) {
    service->PruneStaleEntries();
  }
  return service;
}

bool KoContainerProxyServiceFactory::ServiceIsCreatedWithBrowserContext()
    const {
  // Start fetching the encryptor before container tabs are restored.
  return true;
}
