/* Copyright (c) 2025 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "brave/components/brave_origin/features.h"

#include "brave/components/brave_debloat/buildflags/buildflags.h"
#include "brave/components/brave_origin/buildflags/buildflags.h"

namespace brave_origin::features {

// Debloated non-Origin builds drop the Origin purchase upsell.
BASE_FEATURE(kBraveOrigin,
#if BUILDFLAG(IS_BRAVE_DEBLOATED) && !BUILDFLAG(IS_BRAVE_ORIGIN_BRANDED)
             base::FEATURE_DISABLED_BY_DEFAULT
#else
             base::FEATURE_ENABLED_BY_DEFAULT
#endif
);

}  // namespace brave_origin::features
