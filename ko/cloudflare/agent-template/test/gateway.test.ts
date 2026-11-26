/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

import { describe, expect, it } from 'vitest'
import {
  MAX_TOOL_ROUNDS,
  chat,
  gatewayUrl,
  type McpTools,
} from '../src/gateway'

const env = {
  AI_GATEWAY_ACCOUNT_ID: '0123456789abcdef0123456789abcdef',
  AI_GATEWAY_ID: 'ko-gw',
  AI_GATEWAY_TOKEN: 'aig-token',
  DEFAULT_MODEL: 'openai/gpt-4o-mini',
}

type Call = { url: string; headers: Record<string, string>; body: any }

function fakeFetch(replies: object[]) {
  const calls: Call[] = []
  const impl = (async (url: string, init: RequestInit) => {
    calls.push({
      url,
      headers: init.headers as Record<string, string>,
      body: JSON.parse(String(init.body)),
    })
    const next = replies[Math.min(calls.length - 1, replies.length - 1)]
    return new Response(JSON.stringify(next), { status: 200 })
  }) as unknown as typeof fetch
  return { impl, calls }
}

const text = (t: string) => ({
  choices: [{ message: { role: 'assistant', content: t } }],
})
const toolCall = (name: string, args: object) => ({
  choices: [
    {
      message: {
        role: 'assistant',
        content: null,
        tool_calls: [
          {
            id: 'c1',
            type: 'function',
            function: { name, arguments: JSON.stringify(args) },
          },
        ],
      },
    },
  ],
})

describe('gateway', () => {
  it('builds the compat URL and validates ids', () => {
    expect(gatewayUrl(env)).toBe(
      'https://gateway.ai.cloudflare.com/v1/0123456789abcdef0123456789abcdef/ko-gw/compat/chat/completions',
    )
    expect(() =>
      gatewayUrl({ ...env, AI_GATEWAY_ACCOUNT_ID: '../x' }),
    ).toThrow()
    expect(() => gatewayUrl({ ...env, AI_GATEWAY_ID: '' })).toThrow()
  })

  it('sends the gateway token and default model', async () => {
    const f = fakeFetch([text('hi')])
    const r = await chat(env, [{ role: 'user', content: 'hello' }], {
      fetchImpl: f.impl,
    })
    expect(r.message.content).toBe('hi')
    expect(f.calls[0]!.headers['cf-aig-authorization']).toBe('Bearer aig-token')
    expect(f.calls[0]!.body.model).toBe('openai/gpt-4o-mini')
    expect(f.calls[0]!.body.tools).toBeUndefined()
  })

  it('runs MCP tool calls and returns the final answer', async () => {
    const executed: unknown[] = []
    const tools: McpTools = {
      list: () => [
        {
          name: 'search',
          description: 'search docs',
          inputSchema: { type: 'object' },
          serverId: 'docs',
        },
      ],
      call: async (serverId, name, args) => {
        executed.push({ serverId, name, args })
        return 'result-42'
      },
    }
    const f = fakeFetch([
      toolCall('docs__search', { q: 'x' }),
      text('answer: 42'),
    ])
    const r = await chat(env, [{ role: 'user', content: 'q' }], {
      fetchImpl: f.impl,
      tools,
    })
    expect(executed).toEqual([
      { serverId: 'docs', name: 'search', args: { q: 'x' } },
    ])
    expect(r.message.content).toBe('answer: 42')
    expect(f.calls[0]!.body.tools[0].function.name).toBe('docs__search')
    expect(f.calls[1]!.body.messages.at(-1)).toEqual({
      role: 'tool',
      tool_call_id: 'c1',
      content: 'result-42',
    })
  })

  it('caps tool rounds', async () => {
    const tools: McpTools = {
      list: () => [{ name: 't', inputSchema: {}, serverId: 's' }],
      call: async () => 'again',
    }
    const f = fakeFetch([toolCall('s__t', {})])
    const r = await chat(env, [{ role: 'user', content: 'loop' }], {
      fetchImpl: f.impl,
      tools,
    })
    expect(r.toolRounds).toBe(MAX_TOOL_ROUNDS)
    expect(f.calls.length).toBe(MAX_TOOL_ROUNDS + 1)
    expect(f.calls.at(-1)!.body.tools).toBeUndefined()
  })

  it('surfaces upstream errors without the response body', async () => {
    const impl = (async () =>
      new Response('secret detail', { status: 401 })) as unknown as typeof fetch
    await expect(
      chat(env, [{ role: 'user', content: 'x' }], { fetchImpl: impl }),
    ).rejects.toThrow('AI Gateway returned 401')
  })
})
