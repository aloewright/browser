// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

// K-O branding helpers for branding.js. The "ko" branding component is
// Brave's assets with K-O's files on top, so only K-O-specific files are
// committed under app/theme/ko.

import crypto from 'node:crypto'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { spawnSync } from 'node:child_process'

// Reverse-DNS, at least two labels, no empty labels.
const BUNDLE_ID_RE = /^[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)+$/

const listFiles = (dir) => {
  if (!fs.existsSync(dir)) {
    return []
  }
  return fs
    .readdirSync(dir, { recursive: true, withFileTypes: true })
    .filter((entry) => entry.isFile())
    .map((entry) =>
      path.relative(dir, path.join(entry.parentPath ?? entry.path, entry.name)),
    )
}

const isExcluded = (relPath, excludes) => {
  const parts = relPath.split(path.sep)
  return excludes.some((exclude) =>
    exclude.endsWith('/')
      ? parts[0] === exclude.slice(0, -1)
      : exclude.endsWith('*')
        ? relPath.startsWith(exclude.slice(0, -1))
        : relPath === exclude,
  )
}

// Returns [source, destination] file pairs for `destDir`, taking each file
// from `overlayDir` when present there and from `baseDir` otherwise.
// Excludes: "dir/" drops a top-level directory, "prefix*" a path prefix and
// anything else an exact relative path.
export function overlayPairs({
  baseDir,
  overlayDir,
  destDir,
  baseExcludes = [],
  overlayExcludes = [],
}) {
  const sources = new Map()
  for (const rel of listFiles(baseDir)) {
    if (!isExcluded(rel, baseExcludes)) {
      sources.set(rel, path.join(baseDir, rel))
    }
  }
  for (const rel of listFiles(overlayDir)) {
    if (!isExcluded(rel, overlayExcludes)) {
      sources.set(rel, path.join(overlayDir, rel))
    }
  }
  return [...sources.entries()]
    .filter(([rel]) => path.basename(rel) !== 'README.md')
    .sort(([a], [b]) => a.localeCompare(b))
    .map(([rel, source]) => [source, path.join(destDir, rel)])
}

export function validateBundleId(bundleId) {
  if (!BUNDLE_ID_RE.test(bundleId)) {
    throw new Error(
      `ko_bundle_id "${bundleId}" is not a reverse-DNS bundle id `
        + '(e.g. com.example.ko)',
    )
  }
}

// Builds chrome/app/theme/ko/BRANDING from the release template: non-release
// channels get " <Channel>" product names and a ".<channel>" bundle id suffix,
// matching what //brave/build/ko/branding.gni expects. The Apple team id is
// account specific, so it only comes from the local .env.
export function brandingFileContent(
  template,
  { channel, bundleId, teamId = '' },
) {
  validateBundleId(bundleId)
  if (teamId && !/^[A-Z0-9]{10}$/.test(teamId)) {
    throw new Error(`ko_mac_team_id "${teamId}" is not an Apple team id`)
  }
  const suffix = channel
    ? ` ${channel[0].toUpperCase()}${channel.slice(1)}`
    : ''
  const lines = template.split('\n').map((line) => {
    const eq = line.indexOf('=')
    if (eq === -1) {
      return line
    }
    const key = line.slice(0, eq)
    if (key === 'MAC_BUNDLE_ID') {
      return `${key}=${bundleId}${channel ? `.${channel}` : ''}`
    }
    if (key === 'MAC_TEAM_ID') {
      return `${key}=${teamId}`
    }
    if (key === 'PRODUCT_FULLNAME' || key === 'PRODUCT_SHORTNAME') {
      return `${line}${suffix}`
    }
    return line
  })
  return lines.join('\n')
}

export function writeIfChanged(file, content) {
  if (fs.existsSync(file) && fs.readFileSync(file, 'utf8') === content) {
    return false
  }
  fs.mkdirSync(path.dirname(file), { recursive: true })
  fs.writeFileSync(file, content)
  return true
}

const macVariantDir = (koThemeDir, channel) =>
  path.join(koThemeDir, 'mac', channel === 'development' ? 'development' : '')

export const macAppIconSource = (koThemeDir, channel) =>
  path.join(macVariantDir(koThemeDir, channel), 'app.icns')

const hashTree = (dir) => {
  const hash = crypto.createHash('sha256')
  for (const rel of listFiles(dir).sort()) {
    hash.update(rel)
    hash.update(fs.readFileSync(path.join(dir, rel)))
  }
  return hash.digest('hex')
}

// Places K-O's compiled asset catalog (which holds the macOS 11+ app icon) at
// `dest`. A committed Assets.car is used as is; otherwise it is compiled from
// Assets.xcassets with Xcode's actool, which only exists on macOS.
export function ensureMacAssetCatalog({ koThemeDir, channel, dest }) {
  const dir = macVariantDir(koThemeDir, channel)
  const committed = path.join(dir, 'Assets.car')
  if (fs.existsSync(committed)) {
    const content = fs.readFileSync(committed)
    if (!fs.existsSync(dest) || !fs.readFileSync(dest).equals(content)) {
      fs.mkdirSync(path.dirname(dest), { recursive: true })
      fs.writeFileSync(dest, content)
    }
    return
  }

  const xcassets = path.join(dir, 'Assets.xcassets')
  const stamp = `${dest}.inputs`
  const inputs = hashTree(xcassets)
  if (
    fs.existsSync(dest)
    && fs.existsSync(stamp)
    && fs.readFileSync(stamp, 'utf8') === inputs
  ) {
    return
  }
  if (process.platform !== 'darwin') {
    throw new Error(
      `K-O needs ${committed}; compile ${xcassets} on macOS `
        + '(xcrun actool) or commit the Assets.car it produces.',
    )
  }
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'ko-actool-'))
  try {
    const result = spawnSync(
      'xcrun',
      [
        'actool',
        '--compile',
        tmp,
        '--platform',
        'macosx',
        '--minimum-deployment-target',
        '12.0',
        '--app-icon',
        'AppIcon',
        '--output-partial-info-plist',
        path.join(tmp, 'partial.plist'),
        '--errors',
        '--warnings',
        xcassets,
      ],
      { stdio: 'inherit' },
    )
    const car = path.join(tmp, 'Assets.car')
    if (result.status !== 0 || !fs.existsSync(car)) {
      throw new Error(`actool failed to compile ${xcassets}`)
    }
    fs.mkdirSync(path.dirname(dest), { recursive: true })
    fs.copyFileSync(car, dest)
    fs.writeFileSync(stamp, inputs)
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true })
  }
}

export default {
  overlayPairs,
  validateBundleId,
  brandingFileContent,
  writeIfChanged,
  macAppIconSource,
  ensureMacAssetCatalog,
}
