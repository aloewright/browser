/* Copyright (c) 2021 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "brave/components/skus/common/features.h"

#include "brave/components/ko/buildflags/buildflags.h"
#include "build/build_config.h"

namespace skus::features {

namespace {
// K-O has no Brave subscriptions; the service is also compiled inert there.
constexpr base::FeatureState kSkusFeatureDefault =
#if BUILDFLAG(IS_KO)
    base::FEATURE_DISABLED_BY_DEFAULT;
#else
    base::FEATURE_ENABLED_BY_DEFAULT;
#endif
}  // namespace

BASE_FEATURE(kSkusFeature, kSkusFeatureDefault);

}  // namespace skus::features
