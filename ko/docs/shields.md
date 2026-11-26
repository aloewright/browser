# K-O Shields: built-in ad and tracker blocking

> **Not compiled yet.** Like the rest of K-O, this was written without a
> Chromium checkout. The Python scripts and their tests run; the C++ and
> WebUI changes are statically checked only.

K-O blocks ads and trackers from the first launch, even when Brave's
component updater refuses keyless requests, and the blocking cannot be
switched off quietly. Everything here is behind `is_ko` (C++
`BUILDFLAG(IS_KO)`, upstream overrides `KO_BUILD`); stock and debloated
builds behave as before.

## What you get

| Setting | K-O default | Can the user change it? |
| --- | --- | --- |
| Shields | On for every site | Per site only |
| Trackers & ads | Aggressive (third- and first-party) | Per site only |
| HTTPS upgrade | Strict (HTTP interstitial) | Per site only |
| Fingerprinting | Standard | Per site only |
| Core filter lists | Always on | No |

"Per site only" becomes "no" when the build sets
`ko_shields_strict_enforcement = true`.

## Filter lists

### Embedded snapshot

`ko/shields/lists/` holds a dated snapshot that is compiled into
`brave_resources.pak` (`ko/shields/ko_shields_resources.grd`, brotli
compressed). `lists/manifest.json` records, for every file, the source URL, the
upstream and normalised SHA-256, the size, the rule count, the engine it loads
into and, where it applies, the Brave catalog entry it stands in for.

| Source | Lists | Stands in for |
| --- | --- | --- |
| Brave default list | uBO filters (+ yearly), uBO badware, resource abuse, unbreak, quick fixes, link shorteners, EasyList, URLhaus, Brave unbreak / specific / social / sugarcoat | `default` |
| Brave privacy list | EasyPrivacy, uBO privacy | `4D715457-…` |
| Brave first-party list | Brave first-party (+ regional) | `E99CBD02-…` |
| Cookie notices | EasyList Cookie, uBO cookie annoyances, Brave cookie rules | `AC023D22-…` |
| Mobile app promos | Fanboy mobile notifications | `2F3DCE16-…` |
| Annoyances (off by default) | Fanboy annoyances, uBO other annoyances | `67E792D4-…` |
| K-O extras | Peter Lowe's list, `ko-unbreak` | always on |

Brave's catalog (`brave-list-catalog.json`) and Brave's own scriptlets
(`brave-resources.json`) are embedded too, so the list settings page and
Brave-specific scriptlets work without any component.

**Limitation:** uBlock Origin's scriptlet and redirect resources are built by
Brave's packaging pipeline and are not fetchable as a file. Without the
"Brave Ad Block Resources" component, `+js(...)` rules from uBO lists do
nothing (network blocking is unaffected). Publish a full `resources.json`
through the update channel to fix that (see below).

### How the browser uses them

`KoFiltersProvider` (one per adblock engine) adds a snapshot list only while
its catalog entry is enabled and that entry's Brave component has not been
installed. Once the component arrives, the component's (fresher) data replaces
the snapshot for that entry; K-O extras always load. Component providers no
longer hold the engine back: in K-O a component counts as initialized once
registration with the component updater finishes, installed or not.

When the K-O lists change (new browser version, new channel snapshot) the
cached engine DAT is dropped so the next start rebuilds from the new lists.

Core lists (`default`, privacy, first-party, cookie notices) are always enabled
and their checkboxes are disabled in `brave://settings/shields/filters`.
Ad block only mode is compiled out (`enable_ad_block_only_mode_policies` is
false and the feature defaults off), because it would relax the defaults
through policies.

### Refreshing the snapshot

```bash
python3 ko/shields/update_lists.py            # download, normalise, lint, write
python3 ko/shields/update_lists.py --check    # offline consistency check
python3 ko/shields/update_lists.py --local-only  # after editing local/*.txt
python3 ko/shields/test/test_shields_scripts.py
```

The script downloads over HTTPS only (certificate checks on, no redirect to
`http`), drops comments and blank lines but keeps `!#if` directives, and
refuses lists that contain an unscoped "allow everything" exception such as
`@@*` or `@@||*^$document`. It rewrites `lists/`, the `.grd` and
`ko_embedded_lists.inc`. Commit all of them together.

## Optional update channel

Set these in your local `args.gn` (never commit them):

```gn
ko_filter_lists_url = "https://lists.example.com/k-o"
ko_filter_lists_pubkey = "<64 hex chars>"
```

About two minutes after startup, then every 6 hours (1 hour after a failure),
the browser fetches `manifest.json` and `manifest.json.sig` from that URL
without cookies or credentials and with redirects disabled. It installs a
snapshot only if:

1. the ed25519 signature over `"ko-filter-lists-manifest-v1\n" + manifest`
   verifies with `ko_filter_lists_pubkey`;
2. the manifest parses strictly (ids, file names derived from ids, sizes of
   at most 5 MiB per file, hex digests, engines);
3. its `version` is newer than the embedded snapshot and than any version
   accepted before (anti-rollback; a lost local copy of the last accepted
   version may be restored);
4. it still covers every core catalog list and contains no catalog entry;
5. every file matches its size and SHA-256 and has no unscoped allow-all rule.

Verified files live in `<profile>/ko_filter_lists/` and are checked again
(signature and every digest) at each start and on every read. Anything that
fails falls back to the embedded snapshot and the stored copy is deleted. A
`kind: "resources"` entry replaces the embedded scriptlet resources.

### Publishing

```bash
# Once: create a key. The seed goes to Doppler, the public key to args.gn.
python3 ko/shields/publish_lists.py --generate-key /tmp/ko-lists.key
doppler secrets set KO_FILTER_LISTS_SIGNING_KEY < /tmp/ko-lists.key
rm /tmp/ko-lists.key

# Each release: refresh, sign, upload (lists first, manifest last).
python3 ko/shields/update_lists.py
doppler run -- python3 ko/shields/publish_lists.py \
    --out /tmp/ko-lists --pubkey <hex> \
    --r2-bucket <bucket> --r2-prefix k-o
```

`publish_lists.py` needs the `cryptography` package. Without `--r2-bucket` it
only stages `/tmp/ko-lists`; upload that directory to any HTTPS host. Serve it
with `Cache-Control: no-cache` so clients see new manifests promptly.

To ship full uBO scriptlets, copy `resources.json` from a working Brave
install's "Brave Ad Block Resources" component into the staged directory, add
a `kind: "resources"` entry (id `brave-resources`, file
`brave-resources.json`, its size and SHA-256) to the manifest, then sign.

## Enforcement details

* **Defaults.** The content settings registry registers aggressive cosmetic
  filtering and strict HTTPS upgrades. `BravePrefProvider` ignores writes of a
  global value for Shields, ads, trackers, cosmetic filtering, HTTPS upgrade,
  upgradable resources and fingerprinting, and deletes stored ones at startup
  and whenever the prefs change (sync, profile edits). `DefaultProvider`
  ignores and clears stored defaults for the same types. Enterprise policies
  (`DefaultBrave*Setting`) still apply; they need an admin-installed profile.
* **Per-site exceptions.** Allowed through the Shields panel by default. With
  `ko_shields_strict_enforcement = true`, weakening per-site rules (shields
  down, ads allowed, standard blocking, relaxed HTTPS, fingerprinting off) are
  refused and removed, and the panel shows Shields as managed. Cookie, script
  and webcompat exceptions are not affected.
* **UI.** `brave://settings/shields` disables the default selectors and shows
  a notice; the filter list page disables core lists.
* **Not covered.** User-written custom filters (an explicit action), changing
  features in `brave://flags`, and anyone who can edit the app bundle.

## K-O's own hosts

* `chrome://` pages, including `chrome://ko-workspace` and `chrome://ko-machines`,
  are never filtered: Shields only applies to http(s) pages. Of these, only
  `chrome://ko-workspace` calls `ko-agentd` (`127.0.0.1`, port 47821 by
  default), and those requests are untouched. `chrome://ko-machines` never
  calls agentd, and agentd rejects its `Origin`.
* No filter exception for agentd was added, so websites gain no extra access
  to loopback; agentd's own token, Origin and Host checks still apply.
* Strict HTTPS upgrades exempt `localhost`/loopback. Plain-HTTP private
  addresses (LAN, NetBird `100.64.0.0/10`) show the HTTP interstitial once per
  host when opened as a top-level page.
* `ko-unbreak` exempts Cloudflare Access login pages (`*.cloudflareaccess.com`)
  from all filtering, and first-party requests to per-app `/cdn-cgi/access/`
  endpoints from network blocking.

## GN args

| Arg | Default | Effect |
| --- | --- | --- |
| `ko_filter_lists_url` | `""` | HTTPS base URL of the signed channel; empty disables it |
| `ko_filter_lists_pubkey` | `""` | ed25519 public key (hex) for the channel |
| `ko_shields_strict_enforcement` | `false` | Also refuse per-site relaxation |

Declared in `components/ko/shields/buildflags/buildflags.gni`
(`//brave/components/ko/shields/buildflags`). Strict mode also defines
`KO_SHIELDS_STRICT_BUILD` for upstream-target overrides.

## Code map

* `ko/shields/` – snapshot, scripts, grit target.
* `components/brave_shields/core/browser/ko/` – manifest parsing and
  signature checks, `KoFilterListSource`, `KoFiltersProvider`,
  `KoFilterListUpdater`, unit tests (`ko_filter_lists_unittest.cc`).
* `components/brave_shields/content/browser/ad_block_service.cc` – wiring,
  DAT cache invalidation; started from `BraveBrowserProcessImpl`.
* `components/content_settings/core/browser/brave_content_settings_*` and
  `chromium_src/components/content_settings/core/browser/` – locked defaults.

## Known costs and risks

* About 11 MB of list text is committed per snapshot (roughly 3 MB in the pak).
* Snapshot lists are re-read from the pak on every engine rebuild.
* Only three unit test suites are adapted to K-O:
  `brave_content_settings_pref_provider_unittest`,
  `brave_shields_settings_service_unittest` and `brave_shields_utils_unittest`.
  They skip or adapt cases that assert stock defaults (`IS_KO`) or keep
  per-site relaxations (`ko_shields_strict_enforcement`), and
  `KoLockedShieldsDefaults` and the `KoStrict*` tests cover the K-O behavior.
  Other suites are not adapted yet. For example,
  `brave_shields_data_controller_unittest` expects the stock cosmetic default
  and per-site relaxations, so it is expected to fail in K-O builds.
