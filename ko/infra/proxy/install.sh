#!/bin/sh
# Copyright (c) 2026 The Brave Authors. All rights reserved.
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Installs the pinned gost release (checksum-verified), the config template
# and the systemd unit on a NetBird peer. Run as root. Does not start it.
set -eu
GOST_VERSION=3.3.3
case "$(uname -m)" in
  x86_64) arch=amd64; sum=f4c1255dc892a7014c0ad6e5f1c3573108f7bea4403f04f21873406ab58aa666 ;;
  aarch64 | arm64) arch=arm64; sum=4c704c92df6eeea8a3b08d0de4fd3c44343095a20136b3b6cf334f98d2f174ab ;;
  *) echo "unsupported arch $(uname -m)" >&2; exit 1 ;;
esac
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
tarball="gost_${GOST_VERSION}_linux_${arch}.tar.gz"
curl -fsSL --proto '=https' --tlsv1.2 -o "$tmp/$tarball" \
  "https://github.com/go-gost/gost/releases/download/v${GOST_VERSION}/${tarball}"
echo "$sum  $tmp/$tarball" | sha256sum -c -
tar -xzf "$tmp/$tarball" -C "$tmp" gost
install -m 0755 -o root -g root "$tmp/gost" /usr/local/bin/gost

install -d -m 0755 /etc/ko-proxy
install -m 0644 "$here/gost.yaml.tmpl" /etc/ko-proxy/gost.yaml.tmpl
if [ ! -f /etc/ko-proxy/doppler.env ]; then
  install -m 0600 -o root -g root /dev/null /etc/ko-proxy/doppler.env
  echo "Put DOPPLER_TOKEN=<service token> into /etc/ko-proxy/doppler.env"
fi
install -m 0644 "$here/ko-proxy.service" /etc/systemd/system/ko-proxy.service
systemctl daemon-reload
command -v doppler > /dev/null || echo "Install the Doppler CLI (see ko/docs/infra.md)"
echo "Then: systemctl enable --now ko-proxy"
