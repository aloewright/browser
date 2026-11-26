/* Copyright (c) 2021 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "brave/browser/ui/sidebar/sidebar_service_factory.h"

#include <memory>

#include "base/no_destructor.h"
#include "brave/browser/ui/sidebar/sidebar_utils.h"
#include "brave/components/ko/buildflags/buildflags.h"
#include "brave/components/sidebar/browser/pref_names.h"
#include "brave/components/sidebar/browser/sidebar_service.h"
#include "brave/components/sidebar/common/features.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/first_run/first_run.h"
#include "chrome/browser/profiles/incognito_helpers.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/channel_info.h"
#include "components/keyed_service/content/browser_context_dependency_manager.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/pref_service.h"
#include "components/version_info/channel.h"

#if BUILDFLAG(IS_KO)
#include <algorithm>

#include "components/grit/brave_components_strings.h"
#include "ui/base/l10n/l10n_util.h"
#include "url/gurl.h"
#endif

namespace sidebar {

#if BUILDFLAG(IS_KO)
namespace {

// Set once the K-O workspace web panel was offered, so removing it sticks.
constexpr char kKoWorkspaceItemAddedPref[] = "ko.workspace.sidebar_item_added";
constexpr char kKoWorkspaceUrl[] = "chrome://ko-workspace/";

void MaybeAddKoWorkspaceItem(Profile* profile, SidebarService& service) {
  PrefService* prefs = profile->GetPrefs();
  if (profile->IsOffTheRecord() || profile->IsGuestSession() ||
      prefs->GetBoolean(kKoWorkspaceItemAddedPref)) {
    return;
  }
  prefs->SetBoolean(kKoWorkspaceItemAddedPref, true);
  const GURL url(kKoWorkspaceUrl);
  if (std::ranges::any_of(service.items(), [&url](const SidebarItem& item) {
        return item.url == url;
      })) {
    return;
  }
  service.AddItem(SidebarItem::CreateWebPanelItem(
      url, l10n_util::GetStringUTF16(IDS_KO_WORKSPACE_TITLE)));
}

}  // namespace
#endif  // BUILDFLAG(IS_KO)

// static
SidebarServiceFactory* SidebarServiceFactory::GetInstance() {
  static base::NoDestructor<SidebarServiceFactory> instance;
  return instance.get();
}

SidebarService* SidebarServiceFactory::GetForProfile(Profile* profile) {
  return static_cast<SidebarService*>(
      GetInstance()->GetServiceForBrowserContext(profile, true));
}

SidebarServiceFactory::SidebarServiceFactory()
    : BrowserContextKeyedServiceFactory(
          "SidebarService",
          BrowserContextDependencyManager::GetInstance()) {
  // Early return if the preference is already set or not existed(in test).
  if (!g_browser_process || !g_browser_process->local_state()) {
    return;
  }

  auto* preference = g_browser_process->local_state()->FindPreference(
      kTargetUserForSidebarEnabledTest);
  if (!preference) {
    return;
  }

  // Set target user(new user & feature enabled) flag only once.
  if (g_browser_process->local_state()->GetBoolean(
          kTargetUserForSidebarEnabledTest)) {
    return;
  }

  if (preference->IsDefaultValue()) {
    g_browser_process->local_state()->SetBoolean(
        kTargetUserForSidebarEnabledTest,
        base::FeatureList::IsEnabled(
            sidebar::features::kSidebarShowAlwaysOnStable) &&
            first_run::IsChromeFirstRun());
  }
}

SidebarServiceFactory::~SidebarServiceFactory() = default;

std::vector<SidebarItem::BuiltInItemType>
SidebarServiceFactory::GetBuiltInItemTypesForProfile(Profile* profile) const {
  std::vector<SidebarItem::BuiltInItemType> types;
  for (const auto& type : kDefaultBuiltInItemTypes) {
    if (profile->IsGuestSession()) {
      if (!IsDisabledItemForGuest(type)) {
        types.push_back(type);
      }
      continue;
    }

    // Private(or tor, or enterprise isolated) profile.
    if (profile->IsIncognitoProfile() ||
        profile->IsEnterpriseIsolatedModeProfile()) {
      if (!IsDisabledItemForPrivate(type)) {
        types.push_back(type);
      }
      continue;
    }

    types.push_back(type);
  }

  return types;
}

std::unique_ptr<KeyedService>
SidebarServiceFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  auto* profile = Profile::FromBrowserContext(context);
  auto service = std::make_unique<SidebarService>(
      profile->GetPrefs(), GetBuiltInItemTypesForProfile(profile));
#if BUILDFLAG(IS_KO)
  MaybeAddKoWorkspaceItem(profile, *service);
#endif
  return service;
}

content::BrowserContext* SidebarServiceFactory::GetBrowserContextToUse(
    content::BrowserContext* context) const {
  // sidebar items list is not shared between normal and private windows.
  return GetBrowserContextOwnInstanceInIncognito(context);
}

void SidebarServiceFactory::RegisterProfilePrefs(
    user_prefs::PrefRegistrySyncable* registry) {
  SidebarService::RegisterProfilePrefs(
      registry, GetDefaultShowSidebarOption(chrome::GetChannel()));
#if BUILDFLAG(IS_KO)
  registry->RegisterBooleanPref(kKoWorkspaceItemAddedPref, false);
#endif
}

}  // namespace sidebar
