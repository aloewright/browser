/* Copyright (c) 2019 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "base/environment.h"
#include "brave/browser/metrics/metrics_reporting_util.h"
#include "brave/components/brave_debloat/buildflags/buildflags.h"
#include "brave/components/brave_origin/buildflags/buildflags.h"
#include "brave/components/ko/buildflags/buildflags.h"
#include "brave/components/ko/telemetry/crash_buildflags.h"
#include "chrome/common/channel_info.h"
#include "components/version_info/channel.h"
#include "testing/gtest/include/gtest/gtest.h"

TEST(MetricsUtilTest, DefaultValueTest) {
#if BUILDFLAG(IS_KO)
  // On exactly when a self-hosted crash endpoint is configured.
  EXPECT_EQ(GetDefaultPrefValueForMetricsReporting(),
            BUILDFLAG(KO_CRASH_REPORTING_CONFIGURED));
#elif defined(OFFICIAL_BUILD)
  auto env = base::Environment::Create();

  env->SetVar("CHROME_VERSION_EXTRA", LINUX_CHANNEL_STABLE);
  EXPECT_EQ(version_info::Channel::STABLE, chrome::GetChannel());
  EXPECT_FALSE(GetDefaultPrefValueForMetricsReporting());

  env->SetVar("CHROME_VERSION_EXTRA", LINUX_CHANNEL_BETA);
  EXPECT_EQ(version_info::Channel::BETA, chrome::GetChannel());
#if BUILDFLAG(IS_BRAVE_ORIGIN_BRANDED) || BUILDFLAG(IS_BRAVE_DEBLOATED)
  EXPECT_FALSE(GetDefaultPrefValueForMetricsReporting());
#else
  EXPECT_TRUE(GetDefaultPrefValueForMetricsReporting());
#endif

  env->SetVar("CHROME_VERSION_EXTRA", LINUX_CHANNEL_DEV);
  EXPECT_EQ(version_info::Channel::DEV, chrome::GetChannel());
#if BUILDFLAG(IS_BRAVE_ORIGIN_BRANDED) || BUILDFLAG(IS_BRAVE_DEBLOATED)
  EXPECT_FALSE(GetDefaultPrefValueForMetricsReporting());
#else
  EXPECT_TRUE(GetDefaultPrefValueForMetricsReporting());
#endif

  env->SetVar("CHROME_VERSION_EXTRA", BRAVE_LINUX_CHANNEL_NIGHTLY);
  EXPECT_EQ(version_info::Channel::CANARY, chrome::GetChannel());
#if BUILDFLAG(IS_BRAVE_ORIGIN_BRANDED) || BUILDFLAG(IS_BRAVE_DEBLOATED)
  EXPECT_FALSE(GetDefaultPrefValueForMetricsReporting());
#else
  EXPECT_TRUE(GetDefaultPrefValueForMetricsReporting());
#endif
#else  // OFFICIAL_BUILD
  EXPECT_EQ(version_info::Channel::UNKNOWN, chrome::GetChannel());
  EXPECT_FALSE(GetDefaultPrefValueForMetricsReporting());
#endif
}
