/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

// Chat through the user's AI Gateway (OpenAI-compatible endpoint), with an
// optional MCP tool loop.

export interface GatewayEnv {
  AI_GATEWAY_ACCOUNT_ID?: string
  AI_GATEWAY_ID?: string
  AI_GATEWAY_TOKEN?: string
  DEFAULT_MODEL?: string
}

export interface ChatMessage {
  role: 'system' | 'user' | 'assistant' | 'tool'
  content: string | null
  tool_calls?: ToolCall[]
  tool_call_id?: string
}

export interface ToolCall {
  id: string
  type: 'function'
  function: { name: string; arguments: string }
}

export interface ToolSpec {
  name: string
  description?: string
  inputSchema: unknown
  serverId: string
}

/** The subset of the Agents SDK MCP client the tool loop needs. */
export interface McpTools {
  list(): ToolSpec[]
  call(
    serverId: string,
    name: string,
    args: Record<string, unknown>,
  ): Promise<string>
}

export const MAX_TOOL_ROUNDS = 5
const ID = /^[A-Za-z0-9_-]{1,64}$/

export function gatewayUrl(env: GatewayEnv): string {
  const acct = env.AI_GATEWAY_ACCOUNT_ID ?? ''
  const gw = env.AI_GATEWAY_ID ?? ''
  if (!/^[0-9a-f]{32}$/.test(acct) || !ID.test(gw)) {
    throw new Error('AI Gateway is not configured')
  }
  return `https://gateway.ai.cloudflare.com/v1/${acct}/${gw}/compat/chat/completions`
}

function toolKey(t: ToolSpec): string {
  return `${t.serverId}__${t.name}`.replace(/[^A-Za-z0-9_-]/g, '_').slice(0, 64)
}

export interface ChatResult {
  message: ChatMessage
  transcript: ChatMessage[]
  toolRounds: number
}

/** Runs one chat turn, executing MCP tool calls until a final answer. */
export async function chat(
  env: GatewayEnv,
  messages: ChatMessage[],
  opts: { model?: string; tools?: McpTools; fetchImpl?: typeof fetch } = {},
): Promise<ChatResult> {
  const doFetch = opts.fetchImpl ?? fetch
  const model = opts.model ?? env.DEFAULT_MODEL
  if (!model) throw new Error('no model configured')
  const url = gatewayUrl(env)
  const specs = opts.tools?.list() ?? []
  const byKey = new Map(specs.map((t) => [toolKey(t), t]))
  const tools = specs.map((t) => ({
    type: 'function' as const,
    function: {
      name: toolKey(t),
      description: t.description ?? '',
      parameters: t.inputSchema,
    },
  }))
  const transcript = [...messages]
  for (let round = 0; ; round++) {
    const headers: Record<string, string> = {
      'content-type': 'application/json',
    }
    if (env.AI_GATEWAY_TOKEN)
      headers['cf-aig-authorization'] = `Bearer ${env.AI_GATEWAY_TOKEN}`
    const body: Record<string, unknown> = { model, messages: transcript }
    if (tools.length > 0 && round < MAX_TOOL_ROUNDS) body.tools = tools
    const res = await doFetch(url, {
      method: 'POST',
      headers,
      body: JSON.stringify(body),
    })
    if (!res.ok) {
      throw new Error(`AI Gateway returned ${res.status}`)
    }
    const data = (await res.json()) as {
      choices?: Array<{ message?: ChatMessage }>
    }
    const msg = data.choices?.[0]?.message
    if (!msg) throw new Error('AI Gateway returned no message')
    const reply: ChatMessage = {
      role: 'assistant',
      content: msg.content ?? null,
    }
    if (msg.tool_calls?.length) reply.tool_calls = msg.tool_calls
    transcript.push(reply)
    if (!reply.tool_calls || !opts.tools || round >= MAX_TOOL_ROUNDS) {
      return { message: reply, transcript, toolRounds: round }
    }
    for (const call of reply.tool_calls) {
      const spec = byKey.get(call.function.name)
      let content: string
      if (!spec) {
        content = `unknown tool ${call.function.name}`
      } else {
        try {
          const args = JSON.parse(call.function.arguments || '{}') as Record<
            string,
            unknown
          >
          content = await opts.tools.call(spec.serverId, spec.name, args)
        } catch (e) {
          content = `tool error: ${e instanceof Error ? e.message : String(e)}`
        }
      }
      transcript.push({
        role: 'tool',
        tool_call_id: call.id,
        content: content.slice(0, 32_768),
      })
    }
  }
}
