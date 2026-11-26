#!/bin/sh
# Copyright (c) 2026 The Brave Authors. All rights reserved.
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Installs ko-agentd as a per-user LaunchAgent (macOS). Run as your user:
#   ./install.sh /path/to/ko-agentd
set -eu
src=${1:?usage: install.sh /path/to/ko-agentd}
here=$(cd "$(dirname "$0")" && pwd)
bin_dir="$HOME/.local/bin"
bin="$bin_dir/ko-agentd"
plist="$HOME/Library/LaunchAgents/dev.k-o.agentd.plist"
logs="$HOME/Library/Logs/K-O"

mkdir -p "$bin_dir" "$HOME/Library/LaunchAgents"
mkdir -m 700 -p "$logs"
install -m 0755 "$src" "$bin"
sed -e "s|@HOME@|$HOME|g" -e "s|@BIN@|$bin|g" "$here/dev.k-o.agentd.plist.in" > "$plist.tmp"
plutil -lint "$plist.tmp" > /dev/null
mv "$plist.tmp" "$plist"
chmod 644 "$plist"

"$bin" doctor || echo "doctor reported problems; fix them before relying on the agent"
domain="gui/$(id -u)"
launchctl bootout "$domain/dev.k-o.agentd" 2> /dev/null || true
launchctl bootstrap "$domain" "$plist"
launchctl enable "$domain/dev.k-o.agentd"
echo "ko-agentd loaded; logs in $logs/agentd.log"
