# Debloated Brave for macOS

A fork of `brave/brave-core` at tag `v1.100.58` (Chromium 158.0.8127.42),
branch `debloat/v1.100.58`. It removes Brave's optional services at compile
time and keeps a plain Chromium browser with Brave Shields and opt-in Sync.

> **Not compiled.** This branch was written without a Chromium checkout or a
> Mac, so it has never been run through `gn gen` or built. All checks were
> static (reading code, `#if BUILDFLAG(...)` guards, GN deps). Expect to fix
> build errors on the first build.

## What is removed

Compiled out by `is_brave_debloated = true` (commit `3dfb80eab`). Each
feature's GN default changed from `!is_brave_origin_branded` to
`!is_brave_origin_branded && !is_brave_debloated`, the same set that Brave
Origin already turns off:

- Brave Rewards, Brave Ads
- Brave Wallet
- Brave VPN
- Leo (AI chat) and local AI
- Brave News, Brave Talk
- Tor windows
- Playlist, Speedreader, Wayback Machine prompts
- Web Discovery, PSST, email aliases, Brave education pages
- Stats updater (the daily usage ping)

Later commits on this branch work on telemetry (P3A, metrics), variations
(Griffin), auto-update (Sparkle), crash reporting, referrals, the webcompat
reporter, What's New, onboarding and search promos, and Brave Account. See
`git log v1.100.58..` for exactly what landed.

## What is kept, and why

- **Brave Shields.** Ad and tracker blocking, fingerprinting and cookie
  protections, HTTPS upgrade, de-AMP, debounce, and query-parameter filtering.
  These are the reason to use Brave over plain Chromium.
- **Component updater.** It downloads the adblock filter lists, HTTPS-upgrade
  data, and other Shields resources. If it is off, those lists go stale. Do
  not set the `ComponentUpdatesEnabled` policy to `false`.
- **Brave Sync, opt-in.** Nothing connects to the sync server until you create
  or join a sync chain. No policy is needed to keep it off by default. Do not
  set `SyncDisabled`. To self-host, set `BraveSyncUrl`.

## GN args

| Arg                       | Default | Effect                                                                   |
| ------------------------- | ------- | ------------------------------------------------------------------------ |
| `is_brave_debloated`      | `true`  | The debloat. Set it to `false` to build stock Brave from this branch.    |
| `is_brave_origin_branded` | `false` | Unchanged. Brave Origin branding. Already removes the same feature set. |

The arg is declared in `components/brave_debloat/buildflags/buildflags.gni`.
C++ reads it as `BUILDFLAG(IS_BRAVE_DEBLOATED)` from
`brave/components/brave_debloat/buildflags/buildflags.h`
(GN target `//brave/components/brave_debloat/buildflags`).

To override it for one build, pass `--gn is_brave_debloated:false` to
`pnpm run build`.

## Build on macOS

Prerequisites are the same as for upstream Brave: see
[macOS Development Environment](https://github.com/brave/brave-browser/wiki/macOS-Development-Environment)
(Xcode, Node, pnpm, Python).

In this release the build tooling ships inside brave-core itself (see
`package.json` scripts and `build/commands/`). You clone brave-core into
`src/brave` and run the commands from there:

```bash
# 1. Fork brave/brave-core on GitHub, then push this branch to your fork:
git push <your-fork-remote> debloat/v1.100.58

# 2. Fresh checkout from the fork:
mkdir brave-debloat && cd brave-debloat
git clone -b debloat/v1.100.58 https://github.com/<you>/brave-core.git src/brave
cd src/brave

# 3. Fetch Chromium 158.0.8127.42 (pinned in package.json), apply patches, run hooks.
#    This downloads tens of GB.
pnpm run init

# 4. Release build. Signing runs only if --mac_signing_identifier is set.
pnpm run build Release

# 5. Run it.
pnpm start Release
```

`pnpm run build` without `Release` makes a component build. Its channel is
`development`, so its bundle id is `com.brave.Browser.development`.

**Using a `brave-browser` checkout instead.** The older flow uses a
`brave-browser` checkout. You edit its `package.json` and point
`config.projects["brave-core"].repository.url` at your fork. You also set the
ref (`tag` or `branch`) to `debloat/v1.100.58`. Then you run `npm run init` and
`npm run build Release`. This flow was **not verified** for 1.100.x: brave-core's
own `package.json` lists only the `chrome` and `depot_tools` projects. Prefer
the direct checkout above.

**Release builds share stock Brave's identity.** A Release build uses bundle id
`com.brave.Browser` and the profile directory
`~/Library/Application Support/BraveSoftware/Brave-Browser`. Don't run it next
to stock Brave on the same macOS user. An unsigned build also needs a Gatekeeper
override the first time it opens.

## Runtime policies (defence in depth)

`macos/com.brave.Browser.debloat.mobileconfig` is a configuration profile with
payload type `com.brave.Browser`. It locks the same features off at runtime. It
also works on **stock Brave**.

Install it:

```bash
debloat/macos/apply-policies.sh               # validate with plutil -lint, then open
debloat/macos/apply-policies.sh --no-open     # validate only
debloat/macos/apply-policies.sh --bundle-id com.brave.Browser.beta
```

The script registers the profile with macOS but does not install it. To
install it, open System Settings > Privacy & Security > Profiles (on newer
macOS: General > Device Management) and approve it there. Then relaunch Brave
and check `brave://policy`. The script doesn't write
`/Library/Managed Preferences` by hand, because that isn't reliable: macOS
manages that directory from installed profiles.

Policies in the profile:

- **Brave policies.** Each one is defined under
  `components/policy/resources/templates/policy_definitions/BraveSoftware/`:
  `BraveRewardsDisabled`, `BraveWalletDisabled`, `BraveVPNDisabled`,
  `BraveAIChatEnabled`, `BraveLocalAIEnabled`, `BraveNewsDisabled`,
  `BraveTalkDisabled`, `TorDisabled`, `BravePlaylistEnabled`,
  `BraveSpeedreaderEnabled`, `BraveWaybackMachineEnabled`,
  `BraveWebDiscoveryEnabled`, `EmailAliasesEnabled`, `PsstEnabled`,
  `BraveP3AEnabled`, `BraveStatsPingEnabled`.
- **Chromium policies.** These are standard Chromium policies, but there is no
  Chromium checkout here, so they were **not verified** against 158:
  `MetricsReportingEnabled`, `UrlKeyedAnonymizedDataCollectionEnabled`,
  `PromotionsEnabled`, `FeedbackSurveysEnabled`.

On the debloated build, most Brave policies are mapped only when their feature
is compiled in (`browser/policy/brave_simple_policy_map.h`). For removed
features, `brave://policy` shows those policies as unknown or ignored. That is
expected and harmless.

These Shields policies are left out on purpose: `DefaultBrave*Setting`,
`BraveDeAmpEnabled`, `BraveDebouncingEnabled`,
`BraveTrackingQueryParametersFilteringEnabled`,
`BraveGlobalPrivacyControlEnabled`. They would lock settings that you sometimes
need to relax on broken sites. Add them to the profile if you want them locked.

## Rebase onto a new Brave release

```bash
git remote add upstream https://github.com/brave/brave-core.git   # once
git fetch --unshallow upstream          # only if your clone is shallow
git fetch upstream tag v<NEW> --no-tags
git checkout -b debloat/v<NEW> debloat/v1.100.58
git rebase --onto v<NEW> v1.100.58
pnpm run init        # picks up the new Chromium tag from package.json
```

After the rebase, look for changes that need a debloat update:

- New features that Brave Origin turns off but this fork does not yet:
  `git grep -n '!is_brave_origin_branded' -- '*.gn' '*.gni' | grep -v is_brave_debloated`
- New C++ guards: `git grep -n 'IS_BRAVE_ORIGIN_BRANDED'`. Add
  `IS_BRAVE_DEBLOATED` next to them where it applies.
- New policies: `git diff v1.100.58 v<NEW> --stat -- components/policy/resources/templates/policy_definitions/BraveSoftware/`.
  Add them to the profile.

The changes are small default flips and guards, so conflicts should be rare
and local.
