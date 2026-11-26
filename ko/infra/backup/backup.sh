#!/bin/sh
# Copyright (c) 2026 The Brave Authors. All rights reserved.
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Encrypted backups of GlitchTip and PostHog. Dumps are piped straight into
# age (public-key encryption), so plaintext only exists inside containers.
#   BACKUP_DIR=/srv/ko-backups BACKUP_AGE_RECIPIENT=age1... backup.sh
# POSTHOG_DIR: directory holding the PostHog stack (for the ClickHouse disk).
set -eu
: "${BACKUP_DIR:?}" "${BACKUP_AGE_RECIPIENT:?}"
POSTHOG_DIR=${POSTHOG_DIR:-$(dirname "$0")/../posthog}
RETENTION_DAYS=${BACKUP_RETENTION_DAYS:-14}
umask 077

container() {
  docker ps -q --filter "label=com.docker.compose.project=$1" \
    --filter "label=com.docker.compose.service=$2" | head -n1
}

ts=$(date -u +%Y%m%dT%H%M%SZ)
dest="$BACKUP_DIR/$ts"
mkdir -p "$dest"

if c=$(container ko-glitchtip postgres) && [ -n "$c" ]; then
  docker exec "$c" pg_dump -U glitchtip -Fc glitchtip \
    | age -r "$BACKUP_AGE_RECIPIENT" > "$dest/glitchtip.pgdump.age"
fi

if c=$(container ko-posthog db) && [ -n "$c" ]; then
  docker exec "$c" pg_dump -U posthog -Fc posthog \
    | age -r "$BACKUP_AGE_RECIPIENT" > "$dest/posthog-postgres.pgdump.age"
fi

if c=$(container ko-posthog clickhouse) && [ -n "$c" ]; then
  docker exec "$c" clickhouse-client --query \
    "BACKUP DATABASE posthog TO Disk('backups', '$ts.zip')" > /dev/null
  src="$POSTHOG_DIR/backups/clickhouse/$ts.zip"
  age -r "$BACKUP_AGE_RECIPIENT" < "$src" > "$dest/posthog-clickhouse.zip.age"
  rm -f "$src"
fi

find "$BACKUP_DIR" -mindepth 1 -maxdepth 1 -type d -mtime +"$RETENTION_DAYS" -exec rm -rf {} +
echo "backup written to $dest"
