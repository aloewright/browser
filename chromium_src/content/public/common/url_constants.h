/* Copyright (c) 2020 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef BRAVE_CHROMIUM_SRC_CONTENT_PUBLIC_COMMON_URL_CONSTANTS_H_
#define BRAVE_CHROMIUM_SRC_CONTENT_PUBLIC_COMMON_URL_CONSTANTS_H_

#include <array>
#include <string_view>

#include "build/build_config.h"

#if BUILDFLAG(IS_IOS)
// ios cannot include content deps
namespace content {
inline constexpr char kChromeUIScheme[] = "chrome";
}
#else
#include <content/public/common/url_constants.h>  // IWYU pragma: export
#endif

namespace content {
inline constexpr char kBraveUIScheme[] = "brave";

#if defined(KO_BUILD)
// K-O: ko:// is accepted like brave:// and shown in place of chrome://.
inline constexpr char kKoUIScheme[] = "ko";
inline constexpr auto kBraveUIAliasSchemes =
    std::to_array<std::string_view>({kBraveUIScheme, kKoUIScheme});
inline constexpr char kBraveUIDisplayScheme[] = "ko";
inline constexpr char16_t kBraveUIDisplayScheme16[] = u"ko";
#else
inline constexpr auto kBraveUIAliasSchemes =
    std::to_array<std::string_view>({kBraveUIScheme});
inline constexpr char kBraveUIDisplayScheme[] = "brave";
inline constexpr char16_t kBraveUIDisplayScheme16[] = u"brave";
#endif

// True for the (lower-case) schemes that are rewritten to chrome://.
constexpr bool IsBraveUIAliasScheme(std::string_view scheme) {
  for (std::string_view alias : kBraveUIAliasSchemes) {
    if (scheme == alias) {
      return true;
    }
  }
  return false;
}
}  // namespace content

#endif  // BRAVE_CHROMIUM_SRC_CONTENT_PUBLIC_COMMON_URL_CONSTANTS_H_
