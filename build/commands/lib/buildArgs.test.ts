// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

import { getBuildArgs } from './buildArgs.ts'
import type { Config } from './config.ts'

// Only the fields getBuildArgs() reads; everything else stays unset.
function makeConfig(overrides: {
  isOfficialBuild: boolean
  isBraveDebloated: boolean
  forwardedEnvArgs?: Record<string, unknown>
}): Config {
  const config = {
    isAsan: () => false,
    isLsan: () => false,
    isComponentBuild: () => false,
    isOfficialBuild: () => overrides.isOfficialBuild,
    isDebug: () => false,
    isReleaseBuild: () => overrides.isOfficialBuild,
    isBraveReleaseBuild: () => overrides.isOfficialBuild,
    enableCDMHostVerification: () => false,
    shouldSign: () => false,
    forwardEnvConfigVarsToObject: (
      _keys: string[],
      args: Record<string, unknown>,
    ) => Object.assign(args, overrides.forwardedEnvArgs),
    isBraveDebloated: overrides.isBraveDebloated,
    isBraveOriginBranded: false,
    targetOS: 'mac',
    targetArch: 'arm64',
    channel: '',
    srcDir: '/nonexistent/src',
    outputDir: '/nonexistent/src/out/Release',
    extraGnArgs: {},
  }
  return config as unknown as Config
}

describe('getBuildArgs', () => {
  it.each([true, false])('passes is_brave_debloated=%s to GN', (debloated) => {
    const args = getBuildArgs(
      makeConfig({ isOfficialBuild: false, isBraveDebloated: debloated }),
    )
    expect(args.is_brave_debloated).toBe(debloated)
  })

  it('enables the updater for stock official builds', () => {
    const args = getBuildArgs(
      makeConfig({ isOfficialBuild: true, isBraveDebloated: false }),
    )
    expect(args.enable_updater).toBe(true)
  })

  it('leaves the updater off for debloated official builds', () => {
    const args = getBuildArgs(
      makeConfig({ isOfficialBuild: true, isBraveDebloated: true }),
    )
    expect(args.enable_updater).toBeUndefined()
  })

  it('still honours an explicit enable_updater for debloated builds', () => {
    const args = getBuildArgs(
      makeConfig({
        isOfficialBuild: true,
        isBraveDebloated: true,
        forwardedEnvArgs: { enable_updater: true },
      }),
    )
    expect(args.enable_updater).toBe(true)
  })

  it('does not force the updater for non-official builds', () => {
    const args = getBuildArgs(
      makeConfig({ isOfficialBuild: false, isBraveDebloated: false }),
    )
    expect(args.enable_updater).toBeUndefined()
  })
})
