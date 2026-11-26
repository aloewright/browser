/* Copyright (c) 2020 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#if defined(KO_BUILD)
#include <string_view>

#include "brave/components/ko/telemetry/crash_buildflags.h"

// K-O uploads only to the user's own Sentry-compatible minidump endpoint
// (GN arg ko_crash_upload_url). Empty means no upload at all.
static_assert(std::string_view(BUILDFLAG(KO_CRASH_UPLOAD_URL)).empty() ||
                  std::string_view(BUILDFLAG(KO_CRASH_UPLOAD_URL))
                      .starts_with("https://"),
              "ko_crash_upload_url must be https");
#define BRAVE_CRASH_REPORTER_CLIENT_GET_UPLOAD_URL \
  return BUILDFLAG(KO_CRASH_UPLOAD_URL);
// Debloated forks never upload crash reports, whatever the consent pref says.
#elif defined(OFFICIAL_BUILD) && !defined(BRAVE_DEBLOATED_BUILD)
#define BRAVE_CRASH_REPORTER_CLIENT_GET_UPLOAD_URL \
  return "https://cr.brave.com";
#else
#define BRAVE_CRASH_REPORTER_CLIENT_GET_UPLOAD_URL return std::string();
#endif

#include <components/crash/core/app/crash_reporter_client.cc>
#undef BRAVE_CRASH_REPORTER_CLIENT_GET_UPLOAD_URL
