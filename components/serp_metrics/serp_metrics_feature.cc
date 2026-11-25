/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "brave/components/serp_metrics/serp_metrics_feature.h"

#include "brave/components/brave_debloat/buildflags/buildflags.h"

namespace serp_metrics {

#if BUILDFLAG(IS_BRAVE_DEBLOATED)
// Its only consumers are P3A and the usage ping, both off when debloated.
BASE_FEATURE(kSerpMetricsFeature, base::FEATURE_DISABLED_BY_DEFAULT);
#else
BASE_FEATURE(kSerpMetricsFeature, base::FEATURE_ENABLED_BY_DEFAULT);
#endif

}  // namespace serp_metrics
