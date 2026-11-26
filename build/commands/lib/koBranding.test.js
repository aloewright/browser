// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import {
  brandingFileContent,
  overlayPairs,
  validateBundleId,
} from './koBranding.js'

const TEMPLATE = [
  'COMPANY_FULLNAME=K-O',
  'PRODUCT_FULLNAME=K-O',
  'PRODUCT_SHORTNAME=K-O',
  'MAC_BUNDLE_ID=com.ko.browser',
  'MAC_TEAM_ID=',
  '',
].join('\n')

describe('brandingFileContent', () => {
  it('keeps release names and sets the bundle id', () => {
    const out = brandingFileContent(TEMPLATE, {
      channel: '',
      bundleId: 'com.example.ko',
    })
    expect(out).toContain('PRODUCT_FULLNAME=K-O\n')
    expect(out).toContain('MAC_BUNDLE_ID=com.example.ko\n')
    expect(out).toContain('MAC_TEAM_ID=\n')
  })

  it('suffixes names and bundle id for other channels', () => {
    const out = brandingFileContent(TEMPLATE, {
      channel: 'development',
      bundleId: 'com.ko.browser',
      teamId: 'ABCDE12345',
    })
    expect(out).toContain('PRODUCT_FULLNAME=K-O Development\n')
    expect(out).toContain('PRODUCT_SHORTNAME=K-O Development\n')
    expect(out).toContain('COMPANY_FULLNAME=K-O\n')
    expect(out).toContain('MAC_BUNDLE_ID=com.ko.browser.development\n')
    expect(out).toContain('MAC_TEAM_ID=ABCDE12345\n')
  })

  it('rejects malformed ids', () => {
    expect(() =>
      brandingFileContent(TEMPLATE, { channel: '', bundleId: 'ko' }),
    ).toThrow(/reverse-DNS/)
    expect(() =>
      brandingFileContent(TEMPLATE, {
        channel: '',
        bundleId: 'com.ko.browser',
        teamId: 'team id',
      }),
    ).toThrow(/team id/)
  })
})

describe('validateBundleId', () => {
  it.each(['com.ko.browser', 'com.example.k-o', 'a.b'])('accepts %s', (id) => {
    expect(() => validateBundleId(id)).not.toThrow()
  })
  it.each(['', 'ko', 'com..ko', '.com.ko', 'com.ko.', 'com.ko/x', 'com ko.x'])(
    'rejects "%s"',
    (id) => {
      expect(() => validateBundleId(id)).toThrow()
    },
  )
})

describe('overlayPairs', () => {
  let root
  const write = (rel, content = rel) => {
    const file = path.join(root, rel)
    fs.mkdirSync(path.dirname(file), { recursive: true })
    fs.writeFileSync(file, content)
  }

  beforeEach(() => {
    root = fs.mkdtempSync(path.join(os.tmpdir(), 'ko-branding-test-'))
    write('base/logo.png')
    write('base/linux/logo_16.png')
    write('base/mac/app.icns')
    write('base/BRANDING.beta')
    write('base/README.md')
    write('ko/logo.png')
    write('ko/source/logo.svg')
    write('ko/BRANDING')
  })

  afterEach(() => {
    fs.rmSync(root, { recursive: true, force: true })
  })

  it('prefers overlay files and applies excludes', () => {
    const pairs = overlayPairs({
      baseDir: path.join(root, 'base'),
      overlayDir: path.join(root, 'ko'),
      destDir: path.join(root, 'out'),
      baseExcludes: ['mac/', 'BRANDING*'],
      overlayExcludes: ['source/', 'BRANDING'],
    }).map(([src, dest]) => [
      path.relative(root, src),
      path.relative(root, dest),
    ])
    expect(pairs).toEqual([
      [
        path.join('base', 'linux', 'logo_16.png'),
        path.join('out', 'linux', 'logo_16.png'),
      ],
      [path.join('ko', 'logo.png'), path.join('out', 'logo.png')],
    ])
  })
})
