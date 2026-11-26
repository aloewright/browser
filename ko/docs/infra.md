# K-O infrastructure

Everything lives in `ko/infra/`: self-hosted GlitchTip and PostHog for
telemetry, encrypted backups, a residential proxy peer for K-O containers,
and the ko-agentd LaunchAgent. All files use placeholders, and nothing here
is deployed.

- Secrets come from Doppler (`doppler run`) and never sit in files in the
  repo.
- Services listen only on a host's NetBird IP (written here as
  `100.64.0.10`), or on 127.0.0.1.
- TLS is terminated by your existing private reverse proxy.

## Doppler

One project, e.g. `ko-infra`, with a config per host (`prd`). The names below
are what the files expect. Generate random values with
`openssl rand -hex 32`.

| Secret | Used by |
| --- | --- |
| `KO_BIND_IP` | GlitchTip and PostHog port binding (the host's NetBird IP) |
| `GLITCHTIP_DB_PASSWORD`, `GLITCHTIP_SECRET_KEY` | GlitchTip |
| `GLITCHTIP_DOMAIN` (`https://glitchtip.example.com`), `GLITCHTIP_FROM_EMAIL`, `GLITCHTIP_EMAIL_URL` | GlitchTip |
| `DOMAIN` (`posthog.example.com`), `POSTHOG_SECRET`, `ENCRYPTION_SALT_KEYS` (`openssl rand -hex 16`) | PostHog |
| `OBJECT_STORAGE_ACCESS_KEY_ID`, `OBJECT_STORAGE_SECRET_ACCESS_KEY`, `BROWSERLESS_SECRET`, `INTERNAL_REQUEST_TOKEN`, `RECORDING_API_JWT_SECRET` | PostHog |
| `TLS_BLOCK` (empty) | PostHog upstream compose |
| `KO_PROXY_SOCKS_ADDR` (`100.64.0.10:1080`), `KO_PROXY_HTTP_ADDR` (`100.64.0.10:8080`), `KO_PROXY_ALLOWED_CIDR` (`100.64.0.0/10`) | Proxy peer |
| `KO_PROXY_USER`, `KO_PROXY_PASSWORD` | Proxy client credentials (what K-O containers use) |
| `BRD_PROXY_ADDR` (`brd.superproxy.io:44445`), `BRD_USERNAME` (`brd-customer-<id>-zone-<zone>`), `BRD_PASSWORD` | Bright Data |
| `BRD_PROXY_DIALER` (`tls`), `BRD_PROXY_TLS_SERVER_NAME` (`brd.superproxy.io`) | Bright Data hop |

Give each host its own read-only service token, limited to its config.

## GlitchTip (`ko/infra/glitchtip/compose.yml`)

GlitchTip 7.0.0 runs as a single process: web, task worker and scheduler.
It uses Postgres 17.11 and Valkey 8.1.11. Every image is pinned by tag and
digest. Postgres and Valkey sit on an `internal: true` network with no route
out. Only `web` publishes a port: `${KO_BIND_IP}:8000`. Sign-up and
organization creation are off after the first user.

```sh
cd ko/infra/glitchtip
doppler run --project ko-infra --config prd -- docker compose up -d
doppler run --project ko-infra --config prd -- \
  docker compose exec web ./manage.py createsuperuser
```

Each `${VAR:?}` makes compose refuse to start when the variable is missing.
Create one project for K-O crash reports and one for ko-agentd:

- Browser: `ko_crash_upload_url` is the project's minidump endpoint, e.g.
  `https://glitchtip.example.com/api/<project>/minidump/?sentry_key=<key>`.
  It is a local GN arg; the default `""` means no uploads.
- ko-agentd: put the project's DSN in the Keychain with
  `ko-agentd secrets set glitchtip-dsn`.

## PostHog (`ko/infra/posthog/`)

PostHog's hobby stack has about 40 services, so it is not copied here.
`setup.sh` checks out the upstream compose files at the commit pinned in
`versions.env` and recreates the layout of upstream's `bin/deploy-hobby`. It
does not generate secrets; those come from Doppler.

`compose.ko.yml` overrides the upstream files:

- Caddy is pinned. Upstream's `0.0.0.0:80/443` ports are replaced with
  `${KO_BIND_IP}:8010` over plain HTTP (`auto_https off`).
- `OPT_OUT_CAPTURE=true`, so the instance sends no telemetry to PostHog.
- ClickHouse gets a `backups` disk.

Upstream also binds some debug ports, on 127.0.0.1 only.

```sh
cd ko/infra/posthog
./setup.sh
D="doppler run --project ko-infra --config prd --"
C="docker compose --env-file versions.env -f docker-compose.yml -f compose.ko.yml"
$D $C pull
$D ./pin-digests.sh > compose.pins.yml    # freezes upstream's :master tags
$D $C -f compose.pins.yml up -d
```

The app images (`posthog/posthog`, `posthog-node`) are pinned to the commit.
Several upstream Rust services use `:master`. Review the first pull, then
keep `compose.pins.yml`. To upgrade, bump `versions.env`, rerun `setup.sh`,
review the upstream diff, pull and pin again.

For the browser, set `ko_posthog_host = "https://posthog.example.com"` and
`ko_posthog_project_key` as local GN args. For ko-agentd, use
`[telemetry.posthog]`. PostHog wants 4+ vCPU and 16 GB RAM.

## Reverse proxy

The private proxy terminates TLS with your internal certificates and
forwards to the NetBird IPs. Caddy example:

```caddy
glitchtip.example.com {
	reverse_proxy 100.64.0.10:8000
}
posthog.example.com {
	reverse_proxy 100.64.0.10:8010
}
```

In NetBird, allow only the proxy peer to reach 8000 and 8010 on the
telemetry host. If the proxy runs on the same host, set `KO_BIND_IP` to
`127.0.0.1`.

## Backups (`ko/infra/backup/`)

`backup.sh` writes the following, each encrypted with `age` to a public key
(`BACKUP_AGE_RECIPIENT`):

- `pg_dump -Fc` of the GlitchTip and PostHog Postgres databases
- a ClickHouse `BACKUP DATABASE posthog` archive

Dumps go straight from the container into `age`; a failed dump fails the
run (`pipefail`) and skips pruning. The ClickHouse archive is
encrypted and its plaintext copy deleted. Backups older than
`BACKUP_RETENTION_DAYS` (default 14) are removed. Containers are found by
compose labels, so the script needs no Doppler access.

Install `backup.sh` to `/opt/ko-infra/backup/`. Then:

- put `BACKUP_DIR`, `BACKUP_AGE_RECIPIENT` and `POSTHOG_DIR` in
  `/etc/ko-infra/backup.env`
- enable `ko-backup.timer` (nightly)

Keep the age identity (private key) offline. Copy `BACKUP_DIR` off the host,
e.g. with restic or rsync to another peer. Restore with `age -d`, then
`pg_restore` or ClickHouse `RESTORE`.

## Residential proxy peer (`ko/infra/proxy/`)

K-O containers can each have a proxy URL, e.g.
`socks5h://<KO_PROXY_USER>:<pass>@100.64.0.10:1080` or
`http://…@100.64.0.10:8080`. That URL points at a NetBird peer running gost
3.3.3. gost accepts only clients from `KO_PROXY_ALLOWED_CIDR` that present the
client credentials. It chains every connection through Bright Data's super
proxy, and the Bright Data credentials stay on the peer. `socks5h` keeps DNS
resolution at the exit, so lookups do not leak.

```sh
sudo ./install.sh        # pinned gost release, sha256-checked; template + unit
# Doppler CLI: https://docs.doppler.com/docs/install-cli
sudo sh -c 'echo "DOPPLER_TOKEN=<service token>" > /etc/ko-proxy/doppler.env'
sudo systemctl enable --now ko-proxy
./selftest.sh /usr/local/bin/gost   # local check of the template, 127.0.0.1 only
```

How `ko-proxy.service` runs gost:

- `DynamicUser` with a strict sandbox and no capabilities.
- Doppler renders `gost.yaml.tmpl` into a one-read FIFO on tmpfs
  (`--mount-max-reads 1`). The credentials never touch disk, and Doppler's
  cache dir is on tmpfs too.
- gost logs at `warn` only, so destinations are not logged.

Bright Data:

- The super proxy port is 44445 since their 2026 certificate migration;
  33335 before that.
- `BRD_PROXY_DIALER=tls` wraps the hop in verified TLS (`secure: true`), so
  the zone credentials do not cross the internet in clear text. If Bright
  Data's endpoint does not accept TLS, gost fails to connect (fail closed).
  Set `tcp` only together with the zone's IP allowlist limited to this
  peer's egress IP.
- Never install Bright Data's CA in K-O or the OS. If a zone intercepts
  TLS, sites fail certificate checks instead of being silently exposed.

In NetBird, allow only the Mac's peer group to reach 1080 and 8080 on the
proxy peer.

## ko-agentd LaunchAgent (`ko/infra/launchd/`)

See [agentd.md](agentd.md#macos-service).
`install.sh /path/to/ko-agentd` installs the agent for the current user.
`newsyslog-ko-agentd.conf` rotates its log.
