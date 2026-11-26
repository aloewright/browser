/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "brave/components/ko/telemetry/ko_analytics_prefs.h"

#include "base/time/time.h"
#include "brave/components/ko/telemetry/analytics_buildflags.h"
#include "components/prefs/pref_registry_simple.h"

namespace ko::analytics {

void RegisterLocalStatePrefs(PrefRegistrySimple* registry) {
  registry->RegisterBooleanPref(prefs::kEnabled,
                                BUILDFLAG(KO_ANALYTICS_CONFIGURED));
  registry->RegisterStringPref(prefs::kInstallId, "");
  registry->RegisterIntegerPref(prefs::kInstallIdPeriod, 0);
  registry->RegisterListPref(prefs::kQueue);
  registry->RegisterDictionaryPref(prefs::kFeatureLastReportedDay);
  registry->RegisterTimePref(prefs::kSessionStart, base::Time());
  registry->RegisterTimePref(prefs::kSessionLastActive, base::Time());
  registry->RegisterBooleanPref(prefs::kSessionCleanExit, false);
}

}  // namespace ko::analytics
