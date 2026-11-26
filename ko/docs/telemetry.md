# K-O telemetry

K-O sends no data to Brave. The debloat already turns off P3A, the usage ping,
Brave crash uploads, Griffin and webcompat reports. K-O adds two optional
channels that report only to servers you run:

| Channel | Build args | Off when | User toggle | Default |
| --- | --- | --- | --- | --- |
| Crash reports (Crashpad minidumps) | `ko_crash_upload_url` | Arg is empty | Settings > Privacy and security > Data collection > **Automatically send crash reports** (also on the welcome page) | On |
| Anonymous usage events | `ko_posthog_host`, `ko_posthog_project_key` | Either arg is empty | Same page > **Send anonymous usage statistics** | On |

If an arg is empty, that channel sends nothing and its toggle is hidden. Each arg must be an `https://` URL, and both PostHog args are
set together or not at all (`gn gen` fails otherwise). Builds with
`is_ko = false` behave like the debloat.

## Configure (local only)

These values identify your servers. Keep them out of git. Put them in
`src/brave/.env`, which git ignores:

```sh
gn_args={"ko_crash_upload_url": "https://glitchtip.example.com/api/1/minidump/?sentry_key=0123456789abcdef0123456789abcdef", "ko_posthog_host": "https://posthog.example.com", "ko_posthog_project_key": "phc_EXAMPLE"}
```

You can also pass them for one build:

```sh
pnpm run build Release \
  --gn 'ko_crash_upload_url:https://glitchtip.example.com/api/1/minidump/?sentry_key=...' \
  --gn ko_posthog_host:https://posthog.example.com \
  --gn ko_posthog_project_key:phc_EXAMPLE
```

The values end up in `out/<build>/args.gn` and in the binary. The minidump
`sentry_key` and the PostHog `phc_` key are write-only ingestion keys, so a
leaked binary lets someone send you junk but not read your data. Rotate them
in GlitchTip or PostHog if that happens.

## Crash reports

### Minidump URL from a GlitchTip DSN

GlitchTip 6.0.8 and later accept Sentry-compatible minidump uploads. Create a
project in GlitchTip, open its settings and copy the DSN:

```text
https://<public_key>@glitchtip.example.com/<project_id>
```

The minidump endpoint for that DSN is:

```text
https://glitchtip.example.com/api/<project_id>/minidump/?sentry_key=<public_key>
```

If the DSN has a path before the project id, keep it:
`https://<key>@example.com/glitchtip/7` becomes
`https://example.com/glitchtip/api/7/minidump/?sentry_key=<key>`. The same
URL format works with self-hosted Sentry.

### Behaviour

- `GetUploadUrl()` returns `ko_crash_upload_url`
  (`chromium_src/components/crash/core/app/crash_reporter_client.cc`). An
  empty arg means Crashpad still writes dumps locally but never uploads.
- Only official (Release) builds upload. Component and debug builds never
  upload, whatever the toggle says.
- Consent is the "Automatically send crash reports" pref
  (`metrics.reporting_enabled`). K-O defaults it to on when an endpoint is
  set, and syncs it into Crashpad at startup, because Crashpad only reads its
  own consent file. The `MetricsReportingEnabled` policy still wins.
- Crashpad rate-limits uploads (about one per hour) and gzips them.
- `brave://crashes` lists local reports and their upload state.

### What a report contains

- The minidump: thread stacks and registers of the crashed process, the list
  of loaded modules with their paths, and some memory around the stacks.
  Memory can hold fragments of whatever the process was working on. That is
  why reports go only to your own server.
- Annotations: `prod`, `ver`, `channel`, `plat`, `ptype`, Crashpad's random
  per-install report client id, enabled features and non-trivial command-line
  switches (Chromium's crash keys), plus K-O's `sentry[release]=k-o@<version>`
  and `sentry[environment]=<channel>`, which GlitchTip uses to group by
  release.
- Not included: K-O blanks Chromium's `url-chunk` and `top-origin` crash keys,
  which would otherwise carry the active page URL and top-level origin.
  Some rare diagnostic dumps (`DumpWithoutCrashing`) add their own crash keys.
  Paths in the module list can include your home directory on Linux.

### Symbols and checking it works

Upload symbols from the build output with the GlitchTip CLI:

```sh
glitchtip-cli debug-files upload out/Release --org <org> --project <project>
```

To test, open `brave://inducebrowsercrashforrealz` in a Release build with the
toggle on. Wait for the upload (it can be rate-limited), then look in
`brave://crashes` and in GlitchTip. If GlitchTip returns 4xx, check the
`sentry_key`, the project id and that the server is 6.0.8 or later.

## Anonymous usage events

`KOAnalyticsService` (`components/ko/telemetry/`) runs in the browser process
and posts to `<ko_posthog_host>/batch/`.

### Events

These are the only events. Anything else is dropped
(`components/ko/telemetry/ko_analytics_event.cc`).

| Event | Properties | When |
| --- | --- | --- |
| `browser_start` | none | Each start |
| `session_length_bucket` | `bucket`: `lt_5m`, `5m_30m`, `30m_2h`, `2h_8h`, `gt_8h` | Next start, for the previous session |
| `crash_free_session` | none | Next start, if the previous session exited cleanly |
| `feature_used` | `feature`: `workspace`, `machines`, `assistant`, `container` | First use of that feature each UTC day |

Every event also carries `app_version` (for example `1.100.58`), `channel`,
`os` (`mac`, `linux`, `windows`) and `distinct_id`. Values are checked against
fixed sets, never free text. Unknown properties are stripped, and an
unexpected value drops the whole event. The queue in Local State is checked
again before each upload.

Never sent: URLs, page titles, hostnames, workspace or container names, search
terms or any page content.

### Identity and privacy

- `distinct_id` is a random 128-bit id. It changes every calendar month (UTC)
  and is deleted when you turn the toggle off.
- Events set `$process_person_profile: false` (no PostHog person profiles) and
  `$geoip_disable: true` (no GeoIP lookup).
- Timestamps are rounded down to the hour.
- PostHog still sees the client IP of the request. In the PostHog project
  settings, turn on **Discard client IP data**.

### Delivery

- Batched: the first upload waits 60 seconds, at most 50 events per request.
- Offline queue: up to 200 events in Local State; the oldest are dropped
  first. It survives restarts.
- Retries: network errors, 408, 429 and 5xx back off exponentially from
  1 minute to 6 hours, with jitter. Any other 4xx drops that batch.
- Requests are https only, send no cookies, never follow redirects and use the
  system network context (not a container proxy).
- Logs show only net and HTTP error codes, never the URL, key or payload.

### Toggle

`ko.analytics.enabled` (Local State) defaults to true only when both PostHog
args are set. Turning it off stops uploads and deletes the queue, the install
id and the session record.

### Recording a feature

Other K-O areas record use of their feature from the UI thread:

```cpp
#include "brave/browser/ko/telemetry/ko_telemetry.h"

ko::RecordFeatureUsed(ko::analytics::Feature::kWorkspace);
```

It does nothing if analytics is not configured or the user turned it off.
To add an event or value, extend the allowlist in `ko_analytics_event.cc` and
its tests, and update this page.
