/* Copyright (c) 2020 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "build/build_config.h"

#if !BUILDFLAG(IS_IOS)
#include "brave/components/content_settings/core/browser/brave_content_settings_utils.h"

#if defined(KO_BUILD)
// K-O also deletes "*,*" rules of its locked non-Shields types; see
// BravePrefProvider::EnsureNoWildcardEntries.
#define BRAVE_SET_WEBSITE_SETTING                                  \
  content_settings::IsShieldsContentSettingsType(content_type_) || \
      (value.is_none() &&                                          \
       content_settings::IsKoLockedShieldsDefault(content_type_)) ||
#else
#define BRAVE_SET_WEBSITE_SETTING \
  content_settings::IsShieldsContentSettingsType(content_type_) ||
#endif  // defined(KO_BUILD)
#else
#define BRAVE_SET_WEBSITE_SETTING
#endif

#include <components/content_settings/core/browser/content_settings_pref.cc>

#undef BRAVE_SET_WEBSITE_SETTING
