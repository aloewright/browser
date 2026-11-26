/* Copyright (c) 2019 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "brave/browser/autocomplete/brave_autocomplete_scheme_classifier.h"

#include <string>

#include "base/strings/string_util.h"
#include "brave/components/constants/pref_names.h"
#include "chrome/browser/profiles/profile.h"
#include "components/prefs/pref_service.h"
#include "content/public/common/url_constants.h"

BraveAutocompleteSchemeClassifier::BraveAutocompleteSchemeClassifier(
    Profile* profile)
    : ChromeAutocompleteSchemeClassifier(profile) {}

BraveAutocompleteSchemeClassifier::~BraveAutocompleteSchemeClassifier() =
    default;

// Without this override, typing brave:// or ko:// URLs runs a search.
metrics::OmniboxInputType
BraveAutocompleteSchemeClassifier::GetInputTypeForScheme(
    const std::string& scheme) const {
  if (scheme.empty()) {
    return metrics::OmniboxInputType::EMPTY;
  }
  if (base::IsStringASCII(scheme) &&
      content::IsBraveUIAliasScheme(base::ToLowerASCII(scheme))) {
    return metrics::OmniboxInputType::URL;
  }

  return ChromeAutocompleteSchemeClassifier::GetInputTypeForScheme(scheme);
}
