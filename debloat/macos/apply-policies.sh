#!/bin/bash
# Copyright (c) 2026 The Brave Authors. All rights reserved.
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at https://mozilla.org/MPL/2.0/.

# Validates the debloat configuration profile and hands it to macOS for
# installation. Writing /Library/Managed Preferences by hand is not used:
# macOS rewrites that directory from installed profiles, so a profile is the
# only reliable way to make Brave read these policies as mandatory.

set -euo pipefail

usage() {
  cat <<EOF
Usage: $(basename "$0") [--bundle-id ID] [--ko] [--no-open]

  --bundle-id ID  Target another Brave bundle id, e.g. com.brave.Browser.beta,
                  com.brave.Browser.nightly or com.brave.Browser.development
                  (non-Release builds). Default: com.brave.Browser.
  --ko            K-O build: leave AI chat (the K-O Assistant) enabled.
  --no-open       Only validate; do not open the profile.
EOF
}

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly SOURCE_PROFILE="${SCRIPT_DIR}/com.brave.Browser.debloat.mobileconfig"
readonly DEFAULT_BUNDLE_ID="com.brave.Browser"

bundle_id="${DEFAULT_BUNDLE_ID}"
do_open=1
ko=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --bundle-id)
      [[ $# -ge 2 ]] || { usage >&2; exit 2; }
      bundle_id="$2"
      shift 2
      ;;
    --ko) ko=1; shift ;;
    --no-open) do_open=0; shift ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
  esac
done

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "error: macOS only (needs plutil and the Profiles UI)." >&2
  exit 1
fi

if [[ ! "${bundle_id}" =~ ^com\.brave\.Browser(\.[A-Za-z0-9]+)*$ ]]; then
  echo "error: unexpected bundle id: ${bundle_id}" >&2
  exit 2
fi

plutil -lint "${SOURCE_PROFILE}"

profile="${SOURCE_PROFILE}"
if [[ "${bundle_id}" != "${DEFAULT_BUNDLE_ID}" || "${ko}" -eq 1 ]]; then
  # A distinct identifier and UUIDs let per-channel profiles coexist.
  variant="debloat"
  display_name="Brave (debloated, ${bundle_id})"
  if [[ "${ko}" -eq 1 ]]; then
    variant="ko"
    display_name="K-O (${bundle_id})"
  fi
  tmp_dir="$(mktemp -d)"
  profile="${tmp_dir}/${bundle_id}.${variant}.mobileconfig"
  cp "${SOURCE_PROFILE}" "${profile}"
  plutil -replace PayloadIdentifier -string "${bundle_id}.${variant}" \
    "${profile}"
  plutil -replace PayloadUUID -string "$(uuidgen)" "${profile}"
  plutil -replace PayloadDisplayName -string "${display_name}" "${profile}"
  plutil -replace PayloadContent.0.PayloadType -string "${bundle_id}" \
    "${profile}"
  plutil -replace PayloadContent.0.PayloadIdentifier \
    -string "${bundle_id}.${variant}.policies" "${profile}"
  plutil -replace PayloadContent.0.PayloadUUID -string "$(uuidgen)" \
    "${profile}"
  if [[ "${ko}" -eq 1 ]]; then
    # K-O's assistant is AI chat; BraveAIChatEnabled=false would turn it off.
    plutil -remove PayloadContent.0.BraveAIChatEnabled "${profile}"
    plutil -replace PayloadDescription -string "Locks off Brave's optional \
services, telemetry and promotions. The K-O Assistant stays available." \
      "${profile}"
  fi
  plutil -lint "${profile}"
fi

payload_type="$(plutil -extract PayloadContent.0.PayloadType raw -o - \
  "${profile}")"
if [[ "${payload_type}" != "${bundle_id}" ]]; then
  echo "error: PayloadType is ${payload_type}, expected ${bundle_id}" >&2
  exit 1
fi

echo "Profile OK: ${profile}"

if [[ "${do_open}" -eq 0 ]]; then
  exit 0
fi

open "${profile}"

cat <<EOF

macOS has registered the profile but it is NOT installed yet. To install:
  1. Open System Settings > Privacy & Security > Profiles (on newer macOS
     releases: General > Device Management).
  2. Double-click the new profile ("Brave (debloated)" or "K-O") and choose
     Install; enter an admin password.
  3. Quit and relaunch Brave, then check brave://policy.

To verify from a terminal:
  sudo profiles show -type configuration
  defaults read "/Library/Managed Preferences/${bundle_id}"
To remove: select the profile in the same pane and click "-".
EOF
