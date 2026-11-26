/* Copyright (c) 2020 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "brave/components/content_settings/core/browser/brave_content_settings_utils.h"
#include "build/build_config.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/pref_value_map.h"

#if defined(KO_BUILD)
// K-O Shields defaults always come from the registry (see
// brave/ko/docs/shields.md); stored values are ignored and discarded.
#define KO_DISCARD_LOCKED_SHIELDS_DEFAULTS                          \
  for (const WebsiteSettingsInfo* info :                            \
       *WebsiteSettingsRegistry::GetInstance()) {                   \
    if (content_settings::IsKoLockedShieldsDefault(info->type())) { \
      prefs_->ClearPref(info->default_value_pref_name());           \
    }                                                               \
  }
#define KO_DEFAULT_PROVIDER_READ_FROM_PREF                        \
  if (content_settings::IsKoLockedShieldsDefault(content_type)) { \
    return base::Value();                                         \
  }
#else
#define KO_DISCARD_LOCKED_SHIELDS_DEFAULTS
#define KO_DEFAULT_PROVIDER_READ_FROM_PREF
#endif  // defined(KO_BUILD)

#if !BUILDFLAG(IS_IOS)
#define BRAVE_DISCARD_OR_MIGRATE_OBSOLETE_PREFERENCES           \
  const std::string& autoplay_pref =                            \
      GetPrefName(ContentSettingsType::AUTOPLAY);               \
  if (IntToContentSetting(prefs_->GetInteger(autoplay_pref)) == \
      ContentSetting::CONTENT_SETTING_ASK) {                    \
    prefs_->ClearPref(autoplay_pref);                           \
  }                                                             \
  KO_DISCARD_LOCKED_SHIELDS_DEFAULTS
#else
#define BRAVE_DISCARD_OR_MIGRATE_OBSOLETE_PREFERENCES \
  KO_DISCARD_LOCKED_SHIELDS_DEFAULTS
#endif

#define BRAVE_DEFAULT_PROVIDER_READ_FROM_PREF          \
  KO_DEFAULT_PROVIDER_READ_FROM_PREF                   \
  if (IsBraveValidDefaultValue(content_type, value)) { \
    return value.Clone();                              \
  }

namespace {

bool IsBraveValidDefaultValue(ContentSettingsType content_type,
                              const base::Value& value) {
  return value.is_dict() &&
         content_settings::IsShieldsContentSettingsType(content_type);
}

}  // namespace

#include <components/content_settings/core/browser/content_settings_default_provider.cc>
#undef BRAVE_DEFAULT_PROVIDER_READ_FROM_PREF
#undef BRAVE_DISCARD_OR_MIGRATE_OBSOLETE_PREFERENCES
#undef KO_DEFAULT_PROVIDER_READ_FROM_PREF
#undef KO_DISCARD_LOCKED_SHIELDS_DEFAULTS
