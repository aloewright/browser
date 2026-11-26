// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#if defined(KO_BUILD)

// Included first so the macro below only rewrites the two calls in
// prefs_tab_helper.cc, not the declaration.
#include "chrome/browser/renderer_preferences_util.h"

#include "chrome/browser/profiles/profile.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/security_principal.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/storage_partition_config.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/common/renderer_preferences/renderer_preferences.h"

namespace {

// Mirror containers::kContainersStoragePartitionDomain and
// containers::kKoContainerProxiesPref; this upstream target can't depend on
// brave/components/containers.
constexpr char kKoContainersPartitionDomain[] = "containers";
constexpr char kKoContainerProxiesPref[] = "ko.containers.proxies";

// K-O: WebRTC in a proxied container may only use the proxy (TCP), never
// direct UDP that would reveal the real IP.
void KoMaybeForceProxiedWebRtc(content::WebContents& web_contents,
                               Profile* profile,
                               blink::RendererPreferences* prefs) {
  const content::StoragePartitionConfig& config =
      web_contents.GetSiteInstance()
          ->GetSecurityPrincipal()
          .GetStoragePartitionConfig();
  if (config.partition_domain() != kKoContainersPartitionDomain) {
    return;
  }
  const PrefService* pref_service = profile->GetPrefs();
  if (!pref_service->FindPreference(kKoContainerProxiesPref) ||
      !pref_service->GetDict(kKoContainerProxiesPref)
           .contains(config.partition_name())) {
    return;
  }
  prefs->webrtc_ip_handling_policy =
      blink::mojom::WebRtcIpHandlingPolicy::kDisableNonProxiedUdp;
  prefs->webrtc_ip_handling_urls.clear();
  prefs->webrtc_local_ips_allowed_urls.clear();
}

}  // namespace

#define UpdateFromSystemSettings(PREFS, PROFILE) \
  UpdateFromSystemSettings(PREFS, PROFILE);      \
  KoMaybeForceProxiedWebRtc(GetWebContents(), PROFILE, PREFS)

#endif  // defined(KO_BUILD)

#include <chrome/browser/ui/prefs/prefs_tab_helper.cc>

#if defined(KO_BUILD)
#undef UpdateFromSystemSettings
#endif
