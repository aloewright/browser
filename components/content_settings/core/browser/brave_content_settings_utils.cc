/* Copyright (c) 2020 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "brave/components/content_settings/core/browser/brave_content_settings_utils.h"

#include <optional>

#include "base/check.h"
#include "base/containers/map_util.h"
#include "base/values.h"
#include "brave/components/brave_shields/core/common/brave_shield_constants.h"
#include "url/gurl.h"

#if defined(KO_BUILD)
#include "brave/components/brave_shields/core/common/brave_shields_settings_values.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/content_settings/core/common/content_settings_utils.h"
#endif

namespace {

bool CanPatternBeConvertedToWildcardSchemeAndPort(
    const ContentSettingsPattern& pattern) {
  // 1. Wildcard is already in the desired state.
  // 2. Our firstParty placeholder shouldn't be converted.
  // 3. Patterns that have file:// scheme.
  // 4. We only want to convert patterns that have a specific host, so something
  // like "http://*:80/*" should be left alone.
  if (pattern == ContentSettingsPattern::Wildcard() ||
      pattern == ContentSettingsPattern::FromString("https://firstParty/*") ||
      pattern.GetSchemeType() == ContentSettingsPattern::SCHEME_FILE ||
      pattern.MatchesAllHosts() || pattern.GetHost().empty()) {
    return false;
  }
  // Check for the case when the scheme is wildcard, but the port isn't.
  if (pattern.GetSchemeType() == ContentSettingsPattern::SCHEME_WILDCARD) {
    GURL check_for_port_url("http://" + pattern.ToString());
    return check_for_port_url.has_port();
  }
  GURL url(pattern.ToString());
  if (!url.is_valid() || url.is_empty() || !url.has_host()) {
    return false;
  }
  if (url.has_scheme()) {
    return !ContentSettingsPattern::IsNonWildcardDomainNonPortScheme(
        url.scheme());
  }
  return url.has_port();
}

}  // namespace

namespace content_settings {

const brave_shields::ShieldsContentSettingsTypes&
GetShieldsContentSettingsTypes() {
  return brave_shields::kShieldsContentSettingsTypes;
}

std::string GetShieldsContentTypeName(const ContentSettingsType& content_type) {
  const auto* name =
      IsShieldsContentSettingsType(content_type)
          ? base::FindOrNull(brave_shields::kShieldsContentTypeNames,
                             content_type)
          : nullptr;
  CHECK(name) << "Content type isn't a shield content type.";
  return *name;
}

bool IsShieldsContentSettingsType(const ContentSettingsType& content_type) {
  return GetShieldsContentSettingsTypes().contains(content_type);
}

bool IsShieldsContentSettingsTypeName(const std::string& content_type_name) {
  for (auto content_type : GetShieldsContentSettingsTypes()) {
    if (GetShieldsContentTypeName(content_type) == content_type_name) {
      return true;
    }
  }
  return false;
}

std::optional<ContentSettingsPattern> ConvertPatternToWildcardSchemeAndPort(
    const ContentSettingsPattern& pattern) {
  if (!CanPatternBeConvertedToWildcardSchemeAndPort(pattern)) {
    return std::nullopt;
  }
  DCHECK(!pattern.GetHost().empty());
  std::optional<ContentSettingsPattern> new_pattern =
      ContentSettingsPattern::FromString("*://" + pattern.GetHost() + "/*");
  return new_pattern;
}

// Returns the full path in the user preferences store to the Brave Shields
// setting identified by it's name (i.e. |name|).
std::string GetShieldsSettingUserPrefsPath(const std::string& name) {
  return std::string("profile.content_settings.exceptions.").append(name);
}

// Extract a SessionModel from |dict[key]|. Will return
// SessionModel::Durable if no model exists.
content_settings::mojom::SessionModel GetSessionModelFromDictionary(
    const base::DictValue& dict,
    const char* key) {
  std::optional<int> model_int = dict.FindInt(key);
  if (!model_int.has_value() ||
      (model_int >
       static_cast<int>(content_settings::mojom::SessionModel::kMaxValue)) ||
      (model_int < 0)) {
    model_int = 0;
  }

  content_settings::mojom::SessionModel session_model =
      static_cast<content_settings::mojom::SessionModel>(model_int.value());
  return session_model;
}

#if defined(KO_BUILD)
bool IsKoLockedShieldsDefault(ContentSettingsType content_type) {
  switch (content_type) {
    case ContentSettingsType::BRAVE_SHIELDS:
    case ContentSettingsType::BRAVE_ADS:
    case ContentSettingsType::BRAVE_TRACKERS:
    case ContentSettingsType::BRAVE_COSMETIC_FILTERING:
    case ContentSettingsType::BRAVE_HTTPS_UPGRADE:
    case ContentSettingsType::BRAVE_HTTP_UPGRADABLE_RESOURCES:
    case ContentSettingsType::BRAVE_FINGERPRINTING_V2:
      return true;
    default:
      return false;
  }
}

bool IsKoWeakeningShieldsValue(ContentSettingsType content_type,
                               const base::Value& value) {
  if (value.is_none()) {
    return false;  // Removing a rule restores the K-O default.
  }
  if (content_type == ContentSettingsType::BRAVE_COSMETIC_FILTERING) {
    // Anything but aggressive (first-party hiding) is weaker.
    const base::DictValue* dict = value.GetIfDict();
    const std::optional<int> type =
        dict ? dict->FindInt(brave_shields::CosmeticFilteringSetting::kName)
             : std::nullopt;
    return type != static_cast<int>(brave_shields::ControlType::BLOCK);
  }
  const ContentSetting setting = ValueToContentSetting(value);
  switch (content_type) {
    case ContentSettingsType::BRAVE_SHIELDS:
      return setting == CONTENT_SETTING_BLOCK;  // Shields down.
    case ContentSettingsType::BRAVE_ADS:
    case ContentSettingsType::BRAVE_TRACKERS:
    case ContentSettingsType::BRAVE_HTTP_UPGRADABLE_RESOURCES:
    case ContentSettingsType::BRAVE_FINGERPRINTING_V2:
      return setting == CONTENT_SETTING_ALLOW;
    case ContentSettingsType::BRAVE_HTTPS_UPGRADE:
      return setting != CONTENT_SETTING_BLOCK;  // Not strict.
    default:
      return false;
  }
}
#endif  // defined(KO_BUILD)

}  // namespace content_settings
