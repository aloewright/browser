# Workspaces (chrome://ko-workspace)

`chrome://ko-workspace` is the browser side of K-O's multi-agent
workspaces. It is a React page built with Leo components, and it works both
as a full tab and in the sidebar's web panel. It talks to the local daemon
`ko-agentd` (see `ko/docs/agentd.md`) and uses the browser only for what
needs it: containers, tabs, captures and workspace apps.

Code: `components/ko/workspace/` (core, mojom, handler, frontend) and
`browser/ui/webui/ko_workspace/`. Only built with `is_ko`.

## Opening it

- On first start, K-O adds a **Workspaces** web panel to the sidebar
  (`chrome://ko-workspace/`). It is added once. If you remove it, it stays
  removed (`ko.workspace.sidebar_item_added`). K-O turns
  `kSidebarWebPanel` on by default.
- The page has **Open in side panel** and **Open in tab** buttons. You can
  also type the URL.

## Connection to ko-agentd

- The base URL is `http://127.0.0.1:<port>`. The port is 47821, or the value
  in `<data>/agentd.port`.
- The token is read from `<data>/agentd.token` on every page load. The page
  requests `/agentd.js`, and C++ reads both files on a background thread and
  answers with `loadTimeData.overrideValues({...})`. The token is never
  stored in prefs or on disk by the browser, and never put in a URL.
- Both files are refused unless they are regular files (not symlinks) owned
  by the user with no group/other permissions. A loose `agentd.port` could
  send the page, and its token, to another local listener. The token must be
  32 to 512 base64url characters (agentd writes 43), optionally followed by
  a newline. The page tells you what to fix (missing, insecure or malformed
  token).
- The K-O Assistant uses the same reader (`ko::ReadAgentdConfig()` in
  `components/ko/common/agentd_config.h`), so both accept the same tokens
  and follow the same port file. Only the Assistant honors the developer
  switch `--ko-agentd-port`.
- The data dir is `~/Library/Application Support/K-O` on macOS, or
  `$XDG_DATA_HOME/k-o` (fallback `~/.local/share/k-o`) on Linux.
- CSP: `connect-src chrome://resources 'self' http://127.0.0.1:<port>`.
  `chrome://resources` is only for Leo icons. Also `frame-src 'none'` and
  `img-src` limited to chrome://resources, chrome://theme, `'self'`, `blob:`
  and `data:` (artifact previews). The CSP port comes from the last read of
  `agentd.port`. If agentd moved, the page reloads once to pick it up.
- agentd only accepts `Origin: chrome://ko-workspace`, checks `Host`, and
  requires the bearer token. SSE is read with `fetch`, because `EventSource`
  cannot send headers.

## Features

| Area | What it does | Where |
| --- | --- | --- |
| Workspaces | List, create, delete (`/v1/workspaces`) | page → agentd |
| Container | Creating a workspace creates a Brave Container and stores its id as `container_id`. A workspace without one gets a **Create** button. | `CreateContainer` (mojo) |
| Workspace tabs | URLs (one per line, kept in `localStorage`), opened as new tabs **in the workspace's container**. http(s) only, at most 20 per click. | `OpenTabsInContainer` |
| Artifacts | List, upload a file (also from Downloads), **Save current page** (MHTML), **Screenshot current tab** (PNG), view, delete | page → agentd; captures via mojo |
| Agent runs | Model picker from `/v1/models` (`claude-code/…`, `codex/…`, `cf-gateway/…`, `nanogpt/…`), prompt, MCP servers from `/v1/mcp/servers`, live output from `/v1/runs/{id}/events`, cancel | page → agentd |
| Cloudflare agent | **Deploy agent** (`POST /v1/cloudflare/agents`), shows the URL | page → agentd |
| Workspace apps | Your own web apps (e.g. behind Cloudflare Access), opened in the side panel or a tab. The browser only navigates; Access does sign-in. | prefs + mojo |

Details:

- **Captures.** The active tab of the window is captured, never this page
  and never non-web pages. The temp MHTML file is deleted right after it is
  read. Both kinds are capped at 20 MiB, below agentd's artifact limit.
- **Viewing.** Images (not SVG) and text are shown inline. Anything else,
  including HTML and MHTML, is downloaded rather than rendered.
- **Untrusted output.** Names and run output from agentd are rendered as
  text.

## Workspace apps

The pref is `ko.workspace.apps` (`[{name, url}]`). On first use it is seeded
from `<data>/workspace_apps.json`; **Reload from file** replaces the list from
the file again. Nothing is committed. Example (placeholders):

```json
{
  "apps": [
    { "name": "Notes", "url": "https://notes.example.com/" },
    { "name": "Board", "url": "https://board.example.com/" }
  ]
}
```

Rules:

- `https` only, no credentials in URLs.
- Names are 1–64 characters. At most 50 apps; duplicates are dropped.
- The file must not be writable by group/other.
- The page can only open URLs that are in the list.

## Bundled extensions (`ko_bundled_extension_dirs`)

GN arg (local `args.gn` only, default `[]`):

```gn
ko_bundled_extension_dirs = [ "/Users/me/k-o-extensions/my-extension" ]
```

What happens at build time
(`components/ko/bundled_extensions/prepare_bundled_extensions.py`):

1. Each directory is copied to `gen/.../ko_extensions/<dir name>`. Hidden
   files and symlinks are skipped.
2. If `manifest.json` has no `"key"`, a fixed key derived from the directory
   name is injected, so the ID is the same on every build and machine. An
   existing `"key"` is kept. The build prints each ID.
3. The copies go into the app bundle next to Brave's own component
   extension files: the framework `Resources/ko_extensions/` on macOS, or
   `resources/ko_extensions/` on Linux.
4. At startup `BraveComponentLoader` loads each one as a component
   extension. The manifests are compiled in, so there is no disk read on
   the UI thread. Their IDs are added to the component extension allowlist
   (`chromium_src/chrome/browser/extensions/component_extensions_allowlist/`).

Requirements:

- The directory name must match `[A-Za-z0-9_-]+`.
- Paths must not end with `/`.
- The manifest needs `name`, `version` and `manifest_version`.

To bring over an extension you already use in Brave (on the Mac):

1. Use the source of an unpacked extension, or copy the installed one from
   `~/Library/Application Support/BraveSoftware/Brave-Browser/Default/Extensions/<id>/<version>/`.
   Delete its `_metadata/` folder; that is Web Store verification data.
2. Store-installed copies already have a `"key"` in `manifest.json`, so they
   keep their old ID. For an unpacked extension, add your own `"key"` to keep
   a specific ID, or let the build inject one.
3. Rename the folder to a plain name (for example `my-extension`) and put
   it outside the repo.
4. Add its absolute path to `ko_bundled_extension_dirs`, run
   `gn gen`/`pnpm run build`, and check the printed ID.

Component extensions run in every profile, cannot be turned off from
`chrome://extensions`, and get component-level trust. Only bundle code you
trust.

## Tests

- `components/ko/common/agentd_config_unittest.cc`: token and port parsing,
  permission and symlink refusal.
- `components/ko/workspace/core/agentd_config_unittest.cc`: `agentd.js`
  content (no token unless it is usable).
- `components/ko/workspace/core/workspace_apps_unittest.cc`: validation and
  both file shapes.
- `components/ko/workspace/browser/ko_workspace_handler_unittest.cc`:
  container name checks, tab opening limited to known containers and http(s),
  one-time seeding, reload, only listed apps open.
- `components/ko/bundled_extensions/prepare_bundled_extensions_test.py`
  (`python3 prepare_bundled_extensions_test.py`): key injection, stable IDs,
  bad input, symlinks.
