#!/bin/sh
# Copyright (c) 2026 The Brave Authors. All rights reserved.
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Prepares the pinned PostHog hobby stack in this directory (the file layout
# upstream's bin/deploy-hobby creates), without generating secrets: those
# come from Doppler. Then start it with:
#   doppler run --project ko-infra --config prd -- \
#     docker compose --env-file versions.env -f docker-compose.yml -f compose.ko.yml up -d
set -eu
cd "$(dirname "$0")"
. ./versions.env

if [ ! -d posthog/.git ]; then
  git clone --filter=blob:none --no-checkout https://github.com/PostHog/posthog.git posthog
fi
git -C posthog fetch --quiet origin "$POSTHOG_COMMIT"
git -C posthog -c advice.detachedHead=false checkout --quiet "$POSTHOG_COMMIT"
test "$(git -C posthog rev-parse HEAD)" = "$POSTHOG_COMMIT"

cp posthog/docker-compose.base.yml docker-compose.base.yml
cp posthog/docker-compose.hobby.yml docker-compose.yml
cp posthog/.env.services .env.services

mkdir -p compose share backups/clickhouse
cat > compose/start <<'SH'
#!/bin/bash
./compose/wait
./bin/migrate
./bin/docker-server
SH
cat > compose/temporal-django-worker <<'SH'
#!/bin/bash
./bin/temporal-django-worker
SH
cat > compose/wait <<'SH'
#!/usr/bin/env python3
import socket
import time

def ready(host, port):
    try:
        with socket.create_connection((host, port), timeout=5):
            return True
    except OSError:
        return False

while not (ready('clickhouse', 9000) and ready('db', 5432)):
    print('Waiting for ClickHouse and Postgres')
    time.sleep(5)
SH
chmod +x compose/start compose/temporal-django-worker compose/wait

# GeoIP database used by ingestion (set KO_POSTHOG_SKIP_GEOIP=1 to skip).
if [ "${KO_POSTHOG_SKIP_GEOIP:-0}" != 1 ] && [ ! -f share/GeoLite2-City.mmdb ]; then
  curl -fsSL --proto '=https' --tlsv1.2 'https://mmdbcdn.posthog.net/' --http1.1 \
    | brotli --decompress > share/GeoLite2-City.mmdb
  printf '{"date": "%s"}\n' "$(date +%Y-%m-%d)" > share/GeoLite2-City.json
  chmod 644 share/GeoLite2-City.mmdb share/GeoLite2-City.json
fi
echo "PostHog ${POSTHOG_COMMIT} prepared. After the first 'up', run ./pin-digests.sh."
