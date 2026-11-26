/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "brave/components/ko/common/ko_paths.h"

#include "build/build_config.h"

#if BUILDFLAG(IS_MAC)
#include "base/base_paths.h"
#include "base/path_service.h"
#elif BUILDFLAG(IS_LINUX)
#include <memory>

#include "base/environment.h"
#include "base/files/file_util.h"
#include "base/nix/xdg_util.h"
#endif

namespace ko {

base::FilePath GetDataDir() {
#if BUILDFLAG(IS_MAC)
  base::FilePath app_support;
  if (!base::PathService::Get(base::DIR_APP_DATA, &app_support)) {
    return base::FilePath();
  }
  return app_support.Append(FILE_PATH_LITERAL("K-O"));
#elif BUILDFLAG(IS_LINUX)
  std::unique_ptr<base::Environment> env = base::Environment::Create();
  base::FilePath data_home =
      base::nix::GetXDGDirectory(env.get(), "XDG_DATA_HOME", ".local/share");
  if (data_home.empty() || !data_home.IsAbsolute()) {
    const base::FilePath home = base::GetHomeDir();
    if (home.empty()) {
      return base::FilePath();
    }
    data_home = home.Append(".local").Append("share");
  }
  return data_home.Append("k-o");
#else
  return base::FilePath();
#endif
}

}  // namespace ko
