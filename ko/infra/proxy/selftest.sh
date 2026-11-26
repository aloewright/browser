#!/bin/sh
# Copyright (c) 2026 The Brave Authors. All rights reserved.
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Local check of gost.yaml.tmpl on 127.0.0.1 only: renders it with dummy
# values (no Doppler), chains to a local stand-in for Bright Data and asserts
# auth on both hops and TLS verification. Needs gost, python3 and curl.
#   ./selftest.sh /usr/local/bin/gost
set -eu
GOST=${1:?usage: selftest.sh /path/to/gost}
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
pids=""
cleanup() {
  for p in $pids; do kill "$p" 2> /dev/null || true; done
  rm -rf "$tmp"
}
trap cleanup EXIT

render() { # $1 = upstream password, $2 = dialer, $3 = upstream addr
  python3 -I - "$here/gost.yaml.tmpl" "$1" "$2" "$3" << 'PY'
import json, re, sys
tmpl, brd_pass, dialer, upstream = sys.argv[1:]
values = {
    'KO_PROXY_SOCKS_ADDR': '127.0.0.1:11080',
    'KO_PROXY_HTTP_ADDR': '127.0.0.1:18080',
    'KO_PROXY_USER': 'ko',
    'KO_PROXY_PASSWORD': 'client-pass',
    'KO_PROXY_ALLOWED_CIDR': '127.0.0.0/8',
    'BRD_PROXY_ADDR': upstream,
    'BRD_USERNAME': 'brd-customer-test-zone-res',
    'BRD_PASSWORD': brd_pass,
    'BRD_PROXY_DIALER': dialer,
    'BRD_PROXY_TLS_SERVER_NAME': 'localhost',
}
print(re.sub(r'\{\{tojson \.([A-Z_]+)\}\}',
             lambda m: json.dumps(values[m.group(1)]), open(tmpl).read()))
PY
}

start_ko() {
  render "$1" "$2" "$3" > "$tmp/gost.yaml"
  "$GOST" -C "$tmp/gost.yaml" > "$tmp/ko.log" 2>&1 &
  ko_pid=$!
  pids="$pids $ko_pid"
  sleep 1
}

stop_ko() {
  kill "$ko_pid"
  wait "$ko_pid" 2> /dev/null || true
}

fetch() {
  curl -s -o /dev/null -w '%{http_code}' --max-time 5 "$@" \
    http://127.0.0.1:18090/ || true
}

mkdir "$tmp/www" && echo ok > "$tmp/www/index.html"
python3 -I -m http.server 18090 --bind 127.0.0.1 --directory "$tmp/www" \
  > /dev/null 2>&1 &
pids="$pids $!"
# Stand-ins for brd.superproxy.io: authenticated HTTP proxy, plain and TLS
# (gost generates a self-signed certificate for the TLS one).
"$GOST" -L "http://brd-customer-test-zone-res:brd-pass@127.0.0.1:18081" \
  > /dev/null 2>&1 &
pids="$pids $!"
"$GOST" -L "http+tls://brd-customer-test-zone-res:brd-pass@127.0.0.1:18443" \
  > /dev/null 2>&1 &
pids="$pids $!"
sleep 1

fail=0
check() {
  if [ "$2" = "$3" ]; then
    echo "ok   $1"
  else
    echo "FAIL $1 (got $2, want $3)"
    fail=1
  fi
}

start_ko brd-pass tcp 127.0.0.1:18081
check "socks5h with client auth" \
  "$(fetch --socks5-hostname ko:client-pass@127.0.0.1:11080)" 200
check "http proxy with client auth" \
  "$(fetch -x http://ko:client-pass@127.0.0.1:18080)" 200
check "socks5 wrong password rejected" \
  "$(fetch --socks5-hostname ko:wrong@127.0.0.1:11080)" 000
check "http proxy without auth rejected" \
  "$(fetch -x http://127.0.0.1:18080)" 407
stop_ko

start_ko wrong-brd-pass tcp 127.0.0.1:18081
check "upstream credentials are required" \
  "$(fetch -x http://ko:client-pass@127.0.0.1:18080)" 502
stop_ko

# The TLS dialer verifies the upstream certificate: an untrusted one fails.
start_ko brd-pass tls 127.0.0.1:18443
check "TLS dialer rejects an untrusted upstream cert" \
  "$(fetch -x http://ko:client-pass@127.0.0.1:18080)" 502
stop_ko

exit $fail
