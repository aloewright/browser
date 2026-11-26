# ko-agentd

`ko-agentd` is the local daemon behind `chrome://ko-workspace` and the K-O
Assistant. It exposes model providers (Claude Code, Codex, Cloudflare AI
Gateway, NanoGPT) through one OpenAI-compatible API. It also stores
workspaces and encrypted artifacts, runs agents with MCP servers, and deploys
per-workspace Cloudflare agents. It is a Rust (edition 2024) binary in
`ko/agentd/`. It is not part of the GN build.

## Files

The daemon keeps everything in the K-O data dir. On macOS that is
`~/Library/Application Support/K-O`. On Linux it is `$XDG_DATA_HOME/k-o`,
falling back to `~/.local/share/k-o`. To use another dir, pass
`--data-dir` or set `KO_AGENTD_DATA_DIR`. This is meant for tests.

| Path | Mode | Contents |
| --- | --- | --- |
| `agentd.toml` | 0600 (no group/world write) | Config. Holds no secrets. |
| `agentd.token` | 0600 | Bearer token. Created on first run. |
| `agentd.sock` | 0600 | Unix socket. Removed on shutdown. |
| `agentd.port` | 0600 | Bound TCP port, written at start. |
| `agentd.lock` | 0600 | `flock` held while serving; a second `serve` on the same dir exits. |
| `agentd.db` | 0600 | SQLite index: workspaces, artifact metadata, runs, MCP servers added through the API. |
| `artifacts/<ws>/<id>.bin` | 0600 | Artifact contents, AES-256-GCM. |
| `runs/<id>.log` | 0600 | Run event log, AES-256-GCM records. |
| `workspaces/<ws>/work/` | 0700 | Default working dir for agent runs. |
| `tmp/` | 0700 | Per-request dirs. Removed after use and at start. |

All dirs are 0700. If the daemon owns a dir with looser permissions, it
tightens it. It refuses to read a token or secrets file whose permissions
are loose. Core dumps are off.

## Security model

- **Listeners.** HTTP listens on `127.0.0.1:47821`. Change the port with
  `server.port` or `serve --port`. A Unix socket is also opened. HTTP/1.1
  only, with a 10 s header read timeout and per-route body limits. Standard
  routes time out after 60 s. Streaming routes (chat, run events) stay open
  until the provider finishes.
- **Auth.** Every route except `GET /v1/health` needs
  `Authorization: Bearer <agentd.token>`. That includes the Unix socket. The
  token is 32 random bytes in base64url. It is compared in constant time over
  SHA-256 digests. `ko-agentd token rotate` replaces it.
- **DNS rebinding.** TCP requests must carry exactly one `Host` header equal to
  `127.0.0.1:<port>` or `localhost:<port>`. An absolute-form target must name
  the same host. Anything else gets 403.
- **Origin / CORS.** Only `chrome://ko-workspace` is allowed. It gets
  `Access-Control-Allow-Origin` (no credentials) and
  `Access-Control-Expose-Headers: X-Ko-Mime-Type`, and preflight answers 204.
  Any other `Origin` (including `null` and `chrome://ko-machines`, which never
  calls agentd) gets 403 with no CORS headers. The assistant calls from the
  browser process and sends no `Origin`. A request without `Origin` but with
  `Sec-Fetch-Site` other than `none` or `same-origin` also gets 403. This
  blocks `<img>`-style probes.
- **Response headers.** Every response carries `nosniff`, `no-store`,
  `no-referrer`, `CSP: default-src 'none'; sandbox`, `X-Frame-Options: DENY`
  and `Vary: Origin`.
- **Secrets.** Secrets live in the macOS Keychain under service
  `K-O ko-agentd`, one item per name. `agentd.toml` refers to them by name.
  Linux has no secure store. `[secrets] backend = "file"` keeps a 0600 JSON
  file, for development and tests only. Without it, the daemon refuses to
  start.
- **Upstreams.** Provider, Cloudflare, GlitchTip, PostHog and MCP HTTP URLs
  must be `https://`. URLs that embed credentials are rejected. Redirects are
  not followed, and the client uses `https_only`. `network.allow_loopback_http`
  allows plain http to 127.0.0.1 only. It exists for the test suite.
- **CLI providers.** The prompt goes on stdin. Argv only carries flags, the
  model and file paths. Each child gets an empty environment, plus an
  allowlist (`HOME`, `PATH`, locale, `XDG_*`, CA bundle, proxy vars), its own
  `TMPDIR` and any configured `pass_env`. `ANTHROPIC_API_KEY` and
  `OPENAI_API_KEY` are never passed, so the CLIs use the subscription login.
  Each child runs in its own process group. Cancelling a run, a timeout or a
  client disconnect sends SIGTERM to the group, then SIGKILL.
- **Encryption at rest.** One 256-bit storage key is kept in the Keychain as
  `storage-key` and created on first start. Every artifact and every run-log
  record is sealed with AES-256-GCM and a random 96-bit nonce. The AAD binds
  the record to its identity, so blobs cannot be swapped, reordered or
  replayed:
  - artifacts: `KOE1 | nonce | ciphertext+tag`, AAD
    `ko-agentd/artifact/v1\0<workspace>\0<artifact>`
  - run logs: `u32 length | KOE1 | nonce | ciphertext+tag` per event, AAD
    `ko-agentd/run-event/v1\0<run>\0<seq>`

  Tampering makes the read fail (500). If the key is missing but encrypted
  data exists, the daemon refuses to start. It does not silently create a new
  key. Metadata (names, tags, mime types, sizes) stays in plaintext SQLite so
  it can be searched.
- **Logs and telemetry.** Logs go to stderr as text or `--log-format json`.
  They never contain headers, bodies or prompts. Error messages pass through a
  scrubber. The scrubber removes every secret value it has loaded, bearer
  tokens, `sk-…`-style keys, URL credentials and `key=value` secrets. Data
  that leaves the machine is also stripped of emails, IPs, 32-hex ids, long
  tokens and home paths.

## Configuration

`ko/agentd/agentd.example.toml` is a complete, commented example. It
contains placeholders only, and a unit test parses it. Unknown keys are
rejected, so a stray `api_key = …` fails to load instead of being ignored.

Secret names used by default:

| Secret | Used for |
| --- | --- |
| `storage-key` | Artifact and run-log encryption (generated) |
| `nanogpt-api-key` | NanoGPT |
| `cf-gateway-aig-token` | `cf-aig-authorization` for an authenticated AI Gateway |
| `cf-gateway-key-<provider>` | Provider key through AI Gateway (optional with BYOK) |
| `cloudflare-api-token` | Worker deploys (Workers Scripts:Edit, account scope) |
| `glitchtip-dsn` | Error reporting (optional) |
| MCP `env_secrets`, `bearer_secret`, `header_secrets` | MCP servers (`mcp-*` for servers added through the API) |

```sh
printf '%s' "$KEY" | ko-agentd secrets set nanogpt-api-key   # or type it; input is hidden
ko-agentd secrets status
ko-agentd doctor
```

## Models

`GET /v1/models` lists the models configured in `agentd.toml`:

| Id | Backend |
| --- | --- |
| `claude-code/<model>` | `claude -p --output-format stream-json --verbose --model <model>` |
| `codex/<model>` | `codex exec --json --skip-git-repo-check --sandbox <mode> --model <model> [--ephemeral] -` |
| `cf-gateway/<provider>/<model>` | `POST https://gateway.ai.cloudflare.com/v1/<account>/<gateway>/compat/chat/completions` with model `<provider>/<model>` |
| `nanogpt/<model>` | `POST https://nano-gpt.com/api/v1/chat/completions` |

Models are listed by provider (Claude Code, Codex, AI Gateway, NanoGPT),
each in its `models` order. The assistant uses the first one as its
default. Each entry looks like this:

```json
{"id": "nanogpt/chatgpt-4o-latest", "object": "model", "created": 0, "owned_by": "nanogpt",
 "display_name": "chatgpt-4o-latest (NanoGPT)",
 "capabilities": {"vision": false, "tools": true}, "context_length": 128000}
```

| Field | Default | Notes |
| --- | --- | --- |
| `display_name` | `<model> (<provider>)`, e.g. `sonnet (Claude Code)` | 1-100 bytes, no control characters |
| `capabilities.tools` | `true` for HTTP providers | Always `false` for `claude-code/*` and `codex/*`: the CLIs run their own tools and never return `tool_calls` |
| `capabilities.vision` | `false` | Always `false` for CLI providers (text only; image parts get 400) |
| `context_length` | `128000` | 700 to 2000000 |

To change them, add a `model_info` table to the provider, keyed by an entry
of its `models` list. Every field is optional:

```toml
[providers.nanogpt]
models = ["chatgpt-4o-latest"]

[providers.nanogpt.model_info."chatgpt-4o-latest"]
display_name = "GPT-4o (NanoGPT)"
vision = true
tools = true
context_length = 128000
```

The daemon refuses to start on a `model_info` key that is not in `models`,
an unknown field, a value out of range, or `tools`/`vision = true` on
`claude-code` or `codex`.

**Default aliases.** `claude-code/default` resolves to the first
`claude-code` model and `codex/default` to the first `codex` model, wherever
a model id is accepted (chat, runs, workspace `default_model`). The browser
sends `claude-code/default` before it has listed models. A model actually
named `default` in `models` takes precedence. The aliases are not listed in
`/v1/models`. If the provider is not configured or is disabled, the request
fails with 404 `model_not_found` and a message naming the provider. HTTP
providers have no alias, since `default` could be a real upstream model.

`POST /v1/chat/completions` follows the OpenAI API. With `stream: true` it
answers with SSE `data:` chunks and a final `data: [DONE]`.

- **HTTP providers** are passed through unchanged (`tools`, `tool_calls`,
  `stream_options` and so on). Only `model` is rewritten, in both directions.
  Upstream errors map to 400, 429 or 502 with a scrubbed message. A missing
  key gives 503, before any request goes out.
- **CLI providers.** The messages are rendered into one prompt: a single user
  message is passed as is, otherwise turns are labelled `[System]`, `[User]`
  and so on. The CLI's events are mapped to `chat.completion.chunk` objects,
  one per assistant text block. The CLIs run their own tools, so tool activity
  is not sent as `tool_calls`, because an OpenAI client would try to execute
  it. Instead the daemon adds a non-standard field:

  ```json
  {"object": "chat.completion.chunk", "choices": [{"index": 0, "delta": {}, "finish_reason": null}],
   "ko_tool_activity": {"type": "tool_use", "id": "toolu_01", "name": "Bash", "input": {"command": "ls"}}}
  ```

  `tool_result` entries carry `tool_use_id`, `is_error` and `content`
  (truncated to 4 KiB). Non-streaming responses collect these entries in a
  `ko_tool_activity` array. Chat requests run in a fresh private dir, and
  Codex chats are always `--sandbox read-only`.

## Workspaces and artifacts

| Method | Path | Notes |
| --- | --- | --- |
| GET | `/v1/workspaces?q=&tag=` | `{"data": [Workspace]}` |
| POST | `/v1/workspaces` | `{name, container_id?, tags?, default_model?, mcp_servers?}` → 201 |
| GET / PATCH / DELETE | `/v1/workspaces/{id}` | PATCH accepts `null` to clear `container_id` / `default_model`. DELETE removes artifacts, run logs and the default work dir. |
| GET | `/v1/workspaces/{id}/artifacts?q=&tag=&mime_type=` | Metadata only |
| POST | `/v1/workspaces/{id}/artifacts` | `{name, mime_type?, tags?, content_text \| content_base64}` → 201 |
| GET | `/v1/workspaces/{id}/artifacts/{aid}` | Metadata + `content_base64`. `?raw=true` returns `application/octet-stream` as an attachment, with the stored type in `X-Ko-Mime-Type`. |
| DELETE | `/v1/workspaces/{id}/artifacts/{aid}` | 204 |
| GET | `/v1/workspaces/{id}/agents` | Configured models (`kind` cli/http, `supports_mcp`, `default`) and the deployed Cloudflare agent |

`Workspace` is `{id, name, container_id, tags, default_model, mcp_servers,
cloudflare_agent, created_at, updated_at}`. Timestamps are RFC 3339, and ids
are UUIDs; anything else in a path gets 404. `container_id` is the Brave
Containers id the browser opens the workspace's tabs in.

## Runs

`POST /v1/workspaces/{id}/runs` takes `{provider?, model, prompt,
mcp_servers?}` and answers 202 with a `Run` object. `model` is a full id, or a
bare model name when `provider` is given. Without either, the workspace's
`default_model` is used. `mcp_servers` defaults to the workspace's list; `[]`
means none. MCP servers need a CLI provider (400 otherwise). Agent runs use
the configured Claude Code `permission_mode` and `allowed_tools`, or the Codex
`sandbox`. They run in `workspaces/<ws>/work`, or under
`[workspaces] workdir_root`. `runs.max_concurrent` limits concurrency and
`runs.timeout_secs` limits run time.

- `GET /v1/runs/{rid}`: `Run` plus `output` (concatenated text) and
  `event_count`. Status is `queued`, `running`, `succeeded`, `failed` or
  `cancelled`. Runs that were active when the daemon died are marked `failed`
  at the next start.
- `GET /v1/runs/{rid}/events`: SSE. The daemon replays the decrypted log, then
  follows live events until `done`. Each event has `id: <seq>`,
  `event: <type>` and `data: {"seq", "ts", "type", "data"}`. Types: `status`,
  `text`, `tool_use`, `tool_result`, `usage`, `error`, `warning`, `done`.
  Send `Last-Event-ID` to resume.
- `POST /v1/runs/{rid}/cancel` (extension) answers 202, or 409 if the run is
  not active.
- `GET /v1/workspaces/{id}/runs` (extension) lists a workspace's runs.

## MCP servers

You define servers in `[mcp.servers.<name>]`. API-added servers go in SQLite.
`GET /v1/mcp/servers` lists both. It shows env var names, header names and
whether each referenced secret is set, but never values. `POST
/v1/mcp/servers` takes `{"name", "type": "http" | "stdio", …}` with secrets
referenced by name. API-added servers may only reference secrets named
`mcp-*`, so a bearer-token holder cannot send provider keys, the Cloudflare
token or `storage-key` to a host of its choosing; any `mcp-*` secret can
still be pointed at any HTTPS URL this way. It refuses stdio servers unless
`mcp.allow_api_stdio_servers = true`, because a stdio server is an arbitrary
command. `DELETE /v1/mcp/servers/{name}` (extension) removes API-added
servers.

How a run passes its servers to each CLI:

- **claude.** The daemon writes `mcp.json` (0600) into the run's 0700 dir under
  `tmp/`, with secrets resolved. It passes `--mcp-config <file>
  --strict-mcp-config`. It deletes the file as soon as the CLI prints its
  first line, and removes the dir when the run ends. Streamable HTTP servers
  use `"type": "http"`.
- **codex.** Codex has no MCP-config-file flag. Servers become `-c
  mcp_servers.<name>.…` overrides, and those contain no secret values.
  Secrets reach the servers through env vars that the overrides only name
  (`env_vars`, `bearer_token_env_var`, `env_http_headers`). The same names
  are added to `shell_environment_policy.exclude`, so Codex's shell tool
  does not see them.

## Cloudflare agents

`POST /v1/cloudflare/agents {"workspace_id"}` deploys
`ko/cloudflare/agent-template` as the worker
`<script_prefix><workspace id without dashes>`. The steps:

1. Reads the script list to see whether the Durable Object migration `v1`
   was already applied.
2. Uploads `PUT /accounts/{account}/workers/scripts/{name}` as multipart:
   - `metadata` with `main_module`, `compatibility_date`, `nodejs_compat`,
     the `KoAgent` Durable Object binding, the Access/gateway vars and
     `AI_GATEWAY_TOKEN` as `secret_text`
   - `index.js` (`application/javascript+module`)
3. Enables `workers.dev` and builds the URL from the account subdomain, or
   uses `cloudflare.url_template`.

It answers 201 with `{workspace_id, script_name, url, deployed_at}` and
stores the result on the workspace. `access_team_domain` and `access_aud`
have no defaults; without them the deploy answers 503.

The bundle is about 2.3 MiB, so it is not committed. `cloudflare.bundle_path`
points at a prebuilt `dist/index.js`. Otherwise `cloudflare.template_dir` is
built at deploy time with `cloudflare.build_command` (default
`bun run build`). The build gets a minimal environment, no secrets and a
5-minute timeout. Run `bun install --frozen-lockfile` in the template once.
If `ko/agentd/assets/ko-agent-worker.js` exists when you build the daemon,
it is embedded and used as a last resort.

## Telemetry

Both channels are off unless configured.

- **GlitchTip.** Set `[telemetry.glitchtip]` and store the DSN in the
  Keychain (`glitchtip-dsn`). The daemon sends Sentry envelopes for 5xx API
  errors and failed runs. Messages have secrets, emails, IP addresses, long
  hex ids and home paths removed. They can still hold provider error text
  and the end of a CLI's stderr, which may name hosts. Reports carry no
  machine name, prompt or model output.
- **PostHog.** Set `[telemetry.posthog]` with `host` and `project_key` (a
  write-only key). The daemon sends `agentd_started`, `chat_completion`
  `{provider, stream}`, `run_finished` `{provider, status, mcp_servers,
  duration_bucket}` and `cloudflare_deploy` `{ok}`. Events use a random
  per-install id with `$process_person_profile: false` and
  `$geoip_disable: true`. They carry no workspace ids or content.

## CLI

```text
ko-agentd [--data-dir DIR] [--log-format text|json] serve [--port N] [--no-unix-socket]
ko-agentd token show | token rotate
ko-agentd secrets set <name>        # value from stdin (hidden on a TTY), 64 KiB max
ko-agentd secrets delete <name> | secrets status
ko-agentd doctor                    # permissions, config, secrets (set/missing), CLI versions, port
```

`serve` stops on SIGINT or SIGTERM. It stops accepting connections, cancels
runs and waits up to 10 s for them to record their final state. Then it
drains connections (10 s) and removes the socket and port file.

## Browser integration

WebUI pages get the base URL (`http://127.0.0.1:<port>`) and the token from
C++ through `loadTimeData`. C++ reads `agentd.token` when the page loads. It
can read `agentd.port` to find a non-default port. Use `fetch` with
`Authorization`, including for SSE (`EventSource` cannot set headers). Never
put the token in a URL.

## macOS service

`ko/infra/launchd/install.sh /path/to/ko-agentd` does four things:

- copies the binary to `~/.local/bin`
- renders `dev.k-o.agentd.plist`: `RunAtLoad`, `KeepAlive` on crash, Aqua
  sessions only so the login Keychain is unlocked, core dumps off, logs in
  `~/Library/Logs/K-O/agentd.log`
- runs `doctor`
- bootstraps it into `gui/<uid>`

For log rotation, use `newsyslog-ko-agentd.conf`. launchd holds the log open,
so restart the agent after a rotation (`launchctl kickstart -k
gui/$(id -u)/dev.k-o.agentd`) or lines written after it are lost.

## Build and test

```sh
cd ko/agentd
cargo fmt --check
cargo clippy --all-targets -- -D warnings
cargo test                                   # unit + integration
cargo test --test cloudflare -- --ignored    # builds and uploads the real template (needs bun install)
cargo build --release                        # target/release/ko-agentd
```

The integration tests start the daemon in-process on an ephemeral port. They
point every upstream at wiremock: NanoGPT, AI Gateway, the Cloudflare API,
GlitchTip and PostHog. They also use the fake `claude` and `codex` scripts in
`tests/fakes/`, which replay the fixtures in `tests/fixtures/`. `tests/e2e.rs`
runs the real binary with the fakes on `PATH`, then checks `secrets`,
`token`, `doctor`, JSON logs and SIGTERM.

## Known limitations

- The daemon was built and tested on Linux only. The macOS Keychain backend
  (keyring 3, `apple-native`) was type-checked for `aarch64-apple-darwin`.
  The full macOS build needs a Mac.
- Claude Code output streams one assistant message at a time, not token by
  token (`--include-partial-messages` is not used).
- `--ephemeral` needs a recent Codex CLI. Set `ephemeral = false` for older
  ones.
