# K-O Assistant

K-O turns Brave's AI chat (Leo) back on as a bring-your-own-model assistant.
`enable_ai_chat` follows `is_ko` (it stays off for plain debloated and Brave
Origin builds); `enable_local_ai` stays off. Everything below is under
`BUILDFLAG(IS_KO)`; `is_ko = false` builds behave like stock Brave.

> **Not compiled yet.** Like the rest of the fork this was written without a
> Chromium checkout. Expect fixes on the first build.

## Where requests go

Only two places:

1. **ko-agentd** on `http://127.0.0.1:<port>`, where the port comes from
   `agentd.port` (default 47821; see the agentd docs). This is the default.
2. **Custom models** the user adds in Settings > K-O Assistant, including the
   Cloudflare AI Gateway and NanoGPT presets. These go straight to the
   provider over HTTPS, not through agentd.

Nothing goes to Brave's AI servers. Compiled out or forced off:

| Removed | How |
| --- | --- |
| Leo models (`GetLeoModels()`), `EngineConsumerConversationAPI`, `ai-chat.bsg` | sources excluded in `core/browser/BUILD.gn`; `GetEngineForModel()` only builds OAI engines |
| Private inference (OHTTP), remote model config | sources excluded; no OHTTP branch in `EngineConsumerOAIRemote` |
| Premium, SKUs | `AIChatCredentialManager` always reports `Inactive` and never binds SKUs; no `SkusServiceFactory` dependency; premium prompt and "Go Premium" hidden |
| Ratings and feedback | `RateMessage`/`SendFeedback` return without a request; no `AIChatFeedbackAPI` instance |
| Conversation sharing | `ShareConversation` fails locally; no share manager; UI hidden |
| Deep research, rich search widgets, NEAR, Brave summary model | `features::Is*Enabled()` return false |
| Brave Search deep links and SERP summaries | throttle never added; staged entries never fetched |
| WebMCP rules component | not registered (`KO_BUILD` in `chromium_src/.../registration.cc`) |

## ko-agentd models

`ModelService` starts with `ko::BuildAgentdModels()`
(`components/ai_chat/core/browser/ko_agentd_models.h`):

- `chat-automatic` ("K-O agentd") is always first, so every "automatic"
  fallback and `GetModels()[0]` land on agentd. It requests agentd's first
  listed model, or `claude-code/default` before agentd has answered.
- One `ko-agentd:<id>` model per entry of agentd's `GET /v1/models`
  (`claude-code/*`, `codex/*`, `cf-gateway/*`, `nanogpt/*`, ...).
- All of them use `http://127.0.0.1:<port>/v1/chat/completions` (see
  [Port](#port)).
- Tool support follows agentd's `tools` capability. Its CLI-backed
  `claude-code/*` and `codex/*` models run their own tools and never hand
  `tool_calls` back, so agentd lists them with `tools: false` (and
  `vision: false`); HTTP providers pass `tool_calls` through. If `tools` is
  missing (an older agentd or cache), `claude-code/*` and `codex/*` get
  tools off and every other id gets tools on. Models with tools on also get
  the `CONTENT_AGENT` capability, so Leo's tools (content agent tools in the
  agent profile, tab management) are offered on them. Models with tools off
  get neither, so they are not offered in the agent profile.
  `chat-automatic` follows the first listed model; before agentd answers it
  is `claude-code/default`, tools off.

The list is fetched when the model service starts, retried with backoff
(15 s to 10 min) while agentd is down, and refreshed every 15 min. The last
list (ids and hints only) is cached in `brave.ai_chat.ko_agentd_models` so
saved conversations keep their model across restarts.

Fields read from each `/v1/models` entry (all but `id` optional):

```json
{"data": [{"id": "claude-code/sonnet[1m]",
           "display_name": "sonnet[1m] (Claude Code)",
           "capabilities": {"vision": false, "tools": false},
           "context_length": 128000},
          {"id": "nanogpt/chatgpt-4o-latest",
           "display_name": "chatgpt-4o-latest (NanoGPT)",
           "capabilities": {"vision": false, "tools": true},
           "context_length": 128000}]}
```

`name` is accepted for `display_name`, `context_window` for
`context_length`, and `vision`/`tools` may sit at the top level. Missing
fields default to: `display_name` the id, `vision` false, `tools` as above,
`context_length` 128000. Ids must match `[A-Za-z0-9._:/@+\[\]-]{1,200}`
(agentd's model-name charset plus `+`; the brackets are for Claude Code
aliases such as `claude-code/sonnet[1m]`). Other ids are skipped, and at
most 200 models are used.

### Errors while streaming

Once a stream has started, agentd reports a failure as
`data: {"error": {"message": "...", "type": "provider_error"}}` followed by
`data: [DONE]`, on a 200 response. A streamed chunk with a top-level `error`
object (or non-empty string) ends the answer: later chunks are dropped and
the request completes with `ConnectionIssue` instead of an empty success.
The error shows "Status code: 200, error code: <type>" (`stream_error` if
the type is missing or not `[A-Za-z0-9._-]{1,64}`); the message itself is
not shown. Like other connection errors it is retried once automatically.
An HTTP error status wins over a streamed error.

### Token

- Read on every fetch from `<data>/agentd.token`: macOS
  `~/Library/Application Support/K-O`, Linux `$XDG_DATA_HOME/k-o` (fallback
  `~/.local/share/k-o`). The Assistant and chrome://ko-workspace share one
  reader, `ko::ReadAgentdConfig()` in `components/ko/common/agentd_config.h`.
- Rejected (fail closed) unless it is a regular file, not a symlink, owned by
  the current user, with no group or other permission bits, at most 1024
  bytes, holding 32 to 512 base64url characters (`[A-Za-z0-9_-]`) with
  optional surrounding ASCII whitespace. agentd writes 43 (32 random bytes,
  unpadded) and a newline. The wider range means a hand-written or longer
  token that agentd accepts still works here.
- Held in memory only, as the API key of the agentd models. It is never
  written to prefs, and the browser strips every API key from models sent to
  WebUI pages (chat, conversation frame, settings).
- Without a token no request is sent to agentd at all: chat requests fail
  with "invalid API key" and `/v1/models` is not called.

### Port

Host always `127.0.0.1`. The port is read from `<data>/agentd.port` in the
same blocking task as the token, with the same file checks (a port file
others can write could point the token at another listener). Without a
valid private port file it is 47821. `--ko-agentd-port=<port>` is a
developer override that wins over the file.

That port is used for `/v1/models` and for every agentd model's chat
endpoint, and is published process-wide (an atomic, safe on any sequence)
for `ko::GetAgentdUrl()` and `ko::IsAgentdEndpoint()`. The fail-closed
check (no token, no request) matches the current port and 47821, where
models built before the first read point. When agentd moves, the next fetch
(retry or 15 min refresh) rebuilds the models on the new port.

### Notes for agentd

- Requests come from the browser process, so they carry no `Origin` header.
  They do carry `Authorization: Bearer <token>`; `Host` is
  `127.0.0.1:<port>`.
- Redirects are never followed (requests fail instead), so the token never
  leaves 127.0.0.1.
- agentd resolves `claude-code/default` to its first configured Claude Code
  model; it is only used before the first `/v1/models` answer. Without
  Claude Code configured it answers 404 `model_not_found`.
- Another local user could bind 47821 while agentd is down (its port file
  is removed on shutdown) and receive the token and prompts. On a
  single-user Mac that user can't read the token file anyway; if that
  matters, keep agentd running (launchd).
- agentd lists `display_name`, `capabilities.vision`, `capabilities.tools`
  and `context_length` for every model. The browser's defaults above only
  matter for an older agentd or a cached list.

## Custom models and presets

Settings > K-O Assistant > Add new model has a **Provider** picker (only in
K-O builds, only when adding):

| Preset | Endpoint | You enter |
| --- | --- | --- |
| Cloudflare AI Gateway | `https://gateway.ai.cloudflare.com/v1/<account id>/<gateway id>/compat/chat/completions` | account id (32 hex), gateway id, model as `provider/model` (e.g. `openai/gpt-5-mini`), key |
| NanoGPT | `https://nano-gpt.com/api/v1/chat/completions` | model id, NanoGPT key |
| Custom endpoint | any HTTPS (or localhost) OpenAI-compatible URL | everything |

For Cloudflare the key is the provider key, or the gateway token if the
gateway stores provider keys for you (it is sent as `Authorization: Bearer`).

Keys are stored OSCrypt-encrypted in the `brave.ai_chat.custom_models` pref
(Keychain-backed on macOS). In K-O the settings page never receives stored
keys: the key field is a password field, and saving an edit with an empty
key keeps the stored one if the endpoint keeps its origin. An edit that
moves the endpoint to another origin drops the stored key, so a hidden key
never follows a new server. To clear a key otherwise, delete and re-add the
model.
Custom models with tool support also get the `CONTENT_AGENT` capability.

Remote endpoints must be HTTPS (`ModelValidator`); no request follows a
redirect.

## UI and strings

User-visible "Leo" strings in the AI chat UI, its settings page, toolbar
button menu, customize-toolbar, NTP and command names have K-O variants via
GRIT `<if expr="is_ko">` (the `is_ko` GRIT define is passed in
`components/resources/BUILD.gn` and `app/BUILD.gn`). Stock strings and their
translations are unchanged; K-O variants are English until translated. The
about and privacy texts describe K-O's providers instead of Brave's servers.
The "Brave" product name is the branding area's.

Not renamed yet: Leo model intro messages, premium and rating strings (not
reachable in K-O), the Brave Search permission prompt, "Ask Leo" in the
omnibox (`omnibox_strings.grdp` is also built by another GRD), and icons.

## Policies

`debloat/macos/com.brave.Browser.debloat.mobileconfig` sets
`BraveAIChatEnabled = false`, which would turn the assistant off. Use the K-O
variant, which drops that key:

```bash
debloat/macos/apply-policies.sh --ko
```

## Tests

- New: `components/ko/common/agentd_config_unittest.cc` (token and port
  file checks, token format), `ko_agentd_models_unittest.cc` (parsing, tool
  defaults, URLs, port-file discovery, fetcher),
  `model_service_ko_unittest.cc` (no Leo models, agentd default, port file,
  keys kept out of pages and prefs, migration) and the K-O stream error
  tests in `oai_api_client_unittest.cc`.
- Left out of K-O builds because they test Leo, premium or Brave's servers:
  `model_service_unittest`, `conversation_handler_unittest`,
  `ai_chat_service_unittest`, `ai_chat_credential_manager_unittest`,
  `ai_chat_metrics_unittest`, the conversation API, OHTTP, deep research and
  remote models tests, the share-conversation test and the Brave Search
  throttle browser test. Two Brave Search summary browser tests and one
  Leo-options OAI test are guarded by `IS_KO`.

## Migration

A default model left over from a stock profile (a Leo key) is cleared on
startup, so the default becomes "K-O agentd".
