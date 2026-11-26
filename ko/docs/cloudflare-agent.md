# Cloudflare workspace agent

`ko/cloudflare/agent-template` is a Cloudflare Worker built on the Agents SDK
(`agents` 0.33.0). Each K-O workspace gets its own worker script. That script
holds one Durable Object (`KoAgent`, SQLite-backed) for the workspace's state,
schedules and MCP connections. `ko-agentd` deploys it (see
[agentd.md](agentd.md#cloudflare-agents)). Nothing is deployed from this repo.

## Request flow

1. `src/gate.ts` validates the `Cf-Access-Jwt-Assertion` header on every
   request (`src/access.ts`, jose):
   - RS256 signature against `https://<team>/cdn-cgi/access/certs`
   - issuer `https://<team>`
   - audience equal to the Access application AUD tag
   - `exp`, `iat` and `sub` present

   It fails closed. When `ACCESS_TEAM_DOMAIN` or `ACCESS_AUD` is empty or
   invalid, every request gets 500. A missing or invalid token gets 403.
2. The verified identity (email, service-token common name or `sub`)
   replaces any client-sent `x-ko-access-identity` header.
3. The request goes to the workspace's Durable Object
   (`getAgentByName(env.KoAgent, KO_WORKSPACE_ID)`).

## Endpoints (inside Access)

| Method | Path | Body | Notes |
| --- | --- | --- | --- |
| GET | `/state` | | Message count, task results, MCP server ids |
| POST | `/chat` | `{message}` or `{messages}`, optional `model` | Calls AI Gateway; runs MCP tool calls (max 5 rounds); persists the last 100 messages |
| DELETE | `/chat` | | Clears history |
| GET / POST | `/schedules` | `{prompt, when, model?}` | `when` = seconds, ISO date or cron; runs `runScheduledPrompt` |
| DELETE | `/schedules/{id}` | | |
| GET / POST | `/mcp` | `{name, url (https), headers?}` | Streamable HTTP/SSE MCP client (`addMcpServer`) |
| DELETE | `/mcp/{id}` | | |

Request bodies are capped at 1 MiB. Chat calls
`https://gateway.ai.cloudflare.com/v1/<AI_GATEWAY_ACCOUNT_ID>/<AI_GATEWAY_ID>/compat/chat/completions`.
When `AI_GATEWAY_TOKEN` is set, the call includes `cf-aig-authorization`. The
worker holds no provider keys, so store them in the gateway (BYOK). Upstream
error bodies are not echoed back. MCP headers you pass to `POST /mcp` are
persisted by the Agents SDK in the Durable Object's storage. Use scoped
tokens.

## Configuration

`wrangler.jsonc` contains placeholders only:

| Var | Set by ko-agentd from |
| --- | --- |
| `ACCESS_TEAM_DOMAIN` | `cloudflare.access_team_domain` (e.g. `example.cloudflareaccess.com`) |
| `ACCESS_AUD` | `cloudflare.access_aud` |
| `AI_GATEWAY_ACCOUNT_ID` | `cloudflare.account_id` |
| `AI_GATEWAY_ID` | `cloudflare.ai_gateway_id`, else `providers.cf-gateway.gateway` |
| `DEFAULT_MODEL` | `cloudflare.default_model` |
| `KO_WORKSPACE_ID` | the workspace id |
| `AI_GATEWAY_TOKEN` (secret) | Keychain `cloudflare.aig_token_secret` |

## Cloudflare setup

1. Zero Trust: create a self-hosted Access application for the agent
   hostname, either `*.<subdomain>.workers.dev` or the custom domain in
   `url_template`. Allow only your identity, plus a service token if a
   machine needs access. Copy the application **AUD tag** and the team domain
   into `agentd.toml`.
2. AI Gateway: create a gateway and turn on authentication. Store the
   provider keys in the gateway (BYOK). Save the gateway token in the
   Keychain:

   ```sh
   ko-agentd secrets set cf-gateway-aig-token
   ```

3. API token: create an account-scoped token with **Workers Scripts:Edit**.
   Save it with `ko-agentd secrets set cloudflare-api-token`.

## Build and test

```sh
cd ko/cloudflare/agent-template
bun install --frozen-lockfile
bun run typecheck     # tsc --noEmit
bun run test          # vitest: Access JWT validation, gate, gateway tool loop
bun run build         # wrangler deploy --dry-run --outdir dist -> dist/index.js
```

Wrangler usage metrics are off: `send_metrics: false` and
`WRANGLER_SEND_METRICS=false`. The unit tests cover the runtime-independent
modules. The Durable Object itself needs workerd, through `bun run dev`. To
deploy by hand instead of through ko-agentd, fill in the vars, run
`wrangler secret put AI_GATEWAY_TOKEN`, then `wrangler deploy`.
