# K-O branding

With `is_ko` set, macOS and Linux builds are named **K-O** and keep their own
identity, separate from any stock Brave install. Windows and Android builds
keep Brave's branding (`is_ko_branded` is false there).

> Not compiled yet. Everything below was checked statically; see
> [Verification](#verification).

## Identity

| Item                   | K-O                                                        | Stock Brave                                              |
| ---------------------- | ---------------------------------------------------------- | -------------------------------------------------------- |
| Product name           | `K-O` (development builds: `K-O Development`)              | `Brave Browser`                                          |
| macOS app / executable | `K-O.app/Contents/MacOS/K-O`, `K-O Framework.framework`    | `Brave Browser.app`                                      |
| macOS bundle id        | GN arg `ko_bundle_id`, default `com.ko.browser`            | `com.brave.Browser`                                      |
| macOS profile          | `~/Library/Application Support/K-O/Browser`                | `~/Library/Application Support/BraveSoftware/Brave-Browser` |
| macOS Keychain item    | `K-O Safe Storage` (account `K-O`)                         | `Brave Safe Storage`                                     |
| Linux profile          | `$XDG_CONFIG_HOME/k-o/browser`                             | `~/.config/BraveSoftware/Brave-Browser`                  |
| Linux package          | `k-o`, desktop file `k-o.desktop`, `/opt/k-o`              | `brave-browser`                                          |
| DMG                    | `K-O.dmg`, volume `K-O`                                    | `Brave Browser.dmg`                                      |
| Internal pages         | shown as `ko://settings`; `ko://`, `brave://`, `chrome://` all work | `brave://`                                      |

Development (component) builds append the channel: bundle id
`com.ko.browser.development`, profile `K-O/Browser-Development`.

The profile lives in a subdirectory of the K-O data dir
(`~/Library/Application Support/K-O`), next to the files ko-agentd keeps in
its root (`agentd.sock`, `agentd.token`, `agentd.toml`). They never overlap.
On Linux the K-O data dir is `$XDG_DATA_HOME/k-o` and the profile is under
`$XDG_CONFIG_HOME`.

## How it is selected

1. `is_ko` (default: follows `is_brave_debloated`) is set in `.env` or with
   `--gn is_ko:true`.
2. `pnpm run build` imports `//brave/build/args/ko/branding_defaults.gni`
   on macOS and Linux, which sets `branding_path_component = "ko"`.
   `//brave/build/ko/branding.gni` asserts that the two agree.
3. `build/commands/lib/branding.js` fills `src/chrome/app/theme/ko`:
   Brave's theme files with the files from `app/theme/ko` on top, plus a
   generated `BRANDING` (product names, bundle id, team id) and the macOS
   icons. Only K-O's own files are committed.
4. `build/config.gni` names the profile dir, Linux package and DMG art from
   `is_ko_branded`; `chromium_src` files switch the Keychain item, Linux
   profile dir, desktop file and scheme on the global `KO_BUILD` define.

## Local configuration

Nothing account-specific is committed. Set these in `src/brave/.env` (or pass
`--gn`):

| Setting          | Default          | Effect                                                         |
| ---------------- | ---------------- | -------------------------------------------------------------- |
| `is_ko`          | `is_brave_debloated` | Turns K-O on.                                              |
| `ko_bundle_id`   | `com.ko.browser` | macOS bundle id (and policy domain). Use your own reverse-DNS id, e.g. `com.example.ko`. |
| `ko_mac_team_id` | empty            | Apple team id written to `BRANDING`, for signing/notarizing. `.env` only. |

`ko_bundle_id` is a GN arg; `branding.js` writes the same value into
`BRANDING`, and GN fails if they differ (re-run `pnpm run build`).

macOS policies use the bundle id as their domain. To use the debloat policy
profile with K-O: `debloat/macos/apply-policies.sh --bundle-id com.ko.browser`.

## Strings

Product strings come from grit, the same files as Brave
(`branding_path_product` stays `brave`). When the build uses the `ko`
branding component, grit rewrites each message as it writes the `.pak`
files, for every `.grd` and every language
(`chromium_src/tools/grit/grit/tool/build.py`, rules in
`script/ko_branding.py`):

- `Brave` becomes `K-O`; `Brave Browser` becomes `K-O`.
- `Brave Shields` becomes `Shields` (the feature keeps its name).
- Names of Brave services and credits stay: Brave Search, Brave Sync,
  Brave Software, Brave Rewards, Brave Wallet, Brave VPN, Brave News, ...
  (see `_KEEP`). The about page keeps "made possible by the Brave open
  source project".
- Brave as the recipient of user data stays Brave ("sent to Brave",
  "shared with Brave", "Brave servers", Brave Privacy Policy/Terms), so
  disclosures never claim that data sent off the device goes to K-O. The
  heuristic (`_RECIPIENT`) is English-only; translations may still say K-O.
- AI chat / Leo strings (`IDS_CHAT_UI_*`, `*AI_CHAT*`, `*_LEO*`) are left to
  the ai-providers area.
- Lowercase `brave`, `brave://` and URLs are untouched.

Translations stay matched because message ids are unchanged; the rewrite is
applied to the translated text too.

Review what changes before building:

```bash
python3 script/ko_branding.py --audit app/brave_strings.grd \
    components/resources/brave_shields_strings.grdp
python3 script/ko_branding_test.py
```

grit does not track `script/ko_branding.py` as an input. After editing the
rules, force string targets to rebuild (for example `touch app/*.grd`).

Strings hard-coded in TypeScript/React (not from grit) are not rewritten.

## `ko://`

`ko://` is a standard, secure scheme registered like `brave://`. Typed or
programmatic `ko://` URLs are rewritten to `chrome://` before navigation, and
`chrome://` pages are displayed (omnibox, copy, status bubble, page titles,
error pages, security origin) as `ko://`. The rewrite happens in the same
browser-side handlers as `brave://`, so `ko://` gets no access that
`chrome://` would not. The constants live in
`chromium_src/content/public/common/url_constants.h`
(`IsBraveUIAliasScheme`, `kBraveUIDisplayScheme`).

## User agent

Unchanged: the Chromium-standard UA string with no K-O token, so K-O does not
stand out. The UA client hints keep Brave's brand list. The product name
appears only in local UI.

## Icons

Sources are SVG under `app/theme/ko/` (`product_logo.svg`, `source/*.svg`).
`script/ko/generate_ko_icons.py` holds the mark's geometry:

```bash
pip install cairosvg   # plus ImageMagick (`convert`)
python3 script/ko/generate_ko_icons.py svg      # rewrite SVG + .icon sources
python3 script/ko/generate_ko_icons.py render   # PNG/ICNS/ICO/DMG art
```

What the macOS build reads (all present):

- `chrome/app/theme/ko/mac/app.icns` (from `app/theme/ko/mac[/development]/app.icns`)
- `chrome/app/theme/ko/mac/Assets.car`: the macOS 11+ app icon
  (`CFBundleIconName = AppIcon`). It is compiled from
  `app/theme/ko/mac[/development]/Assets.xcassets` with `xcrun actool` by
  `branding.js` on the first macOS build, or taken from a committed
  `Assets.car` next to it. It cannot be built on Linux.
- `chrome/app/theme/ko/BRANDING`, `product_logo_*.png`, `product_logo.svg`,
  `default_{100,200}_percent/ko/*` (about page, menus, shortcuts).
- `components/vector_icons/ko/*.icon`,
  `components/omnibox/browser/vector_icons/ko/*.icon` (omnibox, infobars).
- DMG: `build/mac/dmg-background.ko.png`, `dmg[-development].ko.icns`,
  `ds_store/DS_Store.ko`.
- First-run welcome logo: `components/brave_welcome_ui/assets/ko_logo@2x.webp`.

Linux uses `app/theme/ko/linux/*` and `default_100_percent/ko/linux/*`.
`app/theme/ko/win/ko.ico` is generated but not wired (no Windows branding).

## Known limitations

- **Windows and Android** keep Brave's identity (installer GUIDs, profile
  dir, strings).
- **DMG background**: `DS_Store.ko` was generated on Linux with icon positions
  but no background alias. Run `build/mac/ds_store/update_ds_store_files.py`
  on a Mac to add it.
- **Lion icons left**: the Shields button (owned by the Shields area), so
  "the Brave icon" is kept in Shields onboarding text; and the Leo
  `social-brave-release-favicon-fullheight-color` icon in the newer welcome
  page, which is also Brave Search's engine icon and so is not replaced.
- **Importing from Brave**: passwords cannot be imported, since K-O's Keychain
  key differs from Brave's. Export a CSV from Brave and import it instead.
- **Linux policies** are still read from Brave's `/etc/brave/policies`.

## Verification

Static only (no Chromium checkout, no Mac):

- `node --experimental-strip-types` runs of `buildArgs.test.ts` and
  `koBranding.test.js` (jest API shim).
- `python3 script/ko_branding_test.py`; grit hook wiring exercised with
  stand-in grit classes.
- Generated `.icns` parsed back; PNG sizes match Brave's assets.
- GN, grit `<if>` defines and `chromium_src` changes checked against
  Chromium 158.0.8127.42 sources.
