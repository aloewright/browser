/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

// Per-workspace K-O agent: one Durable Object (Agents SDK) per workspace,
// behind Cloudflare Access.

import { Agent, getAgentByName } from 'agents'
import { gate, json } from './gate'
import { trimHistory } from './history'
import {
  chat,
  type ChatMessage,
  type GatewayEnv,
  type McpTools,
} from './gateway'

export interface Env extends GatewayEnv {
  KoAgent: DurableObjectNamespace<KoAgent>
  ACCESS_TEAM_DOMAIN?: string
  ACCESS_AUD?: string
  KO_WORKSPACE_ID?: string
}

interface TaskResult {
  scheduleId: string
  at: string
  ok: boolean
  output: string
}

interface KoState {
  messages: ChatMessage[]
  taskResults: TaskResult[]
  updatedAt: string | null
}

const MAX_MESSAGES = 100
const MAX_TASK_RESULTS = 50
const MAX_BODY = 1024 * 1024

async function readJson(
  request: Request,
): Promise<Record<string, unknown> | null> {
  const len = Number(request.headers.get('content-length') ?? '0')
  if (len > MAX_BODY) return null
  const text = await request.text()
  if (text.length > MAX_BODY) return null
  try {
    const v: unknown = JSON.parse(text)
    return v && typeof v === 'object' && !Array.isArray(v)
      ? (v as Record<string, unknown>)
      : null
  } catch {
    return null
  }
}

function isHttpsUrl(s: unknown): s is string {
  if (typeof s !== 'string') return false
  try {
    return new URL(s).protocol === 'https:'
  } catch {
    return false
  }
}

export class KoAgent extends Agent<Env, KoState> {
  initialState: KoState = { messages: [], taskResults: [], updatedAt: null }

  private mcpTools(): McpTools {
    return {
      list: () =>
        this.mcp.listTools().map((t) => ({
          name: t.name,
          description: t.description,
          inputSchema: t.inputSchema,
          serverId: t.serverId,
        })),
      call: async (serverId, name, args) => {
        const r = await this.mcp.callTool({ serverId, name, arguments: args })
        return JSON.stringify(r.content ?? r)
      },
    }
  }

  private async runChat(
    messages: ChatMessage[],
    model?: string,
  ): Promise<ChatMessage> {
    const result = await chat(this.env, messages, {
      model,
      tools: this.mcpTools(),
    })
    this.setState({
      ...this.state,
      messages: trimHistory(result.transcript, MAX_MESSAGES),
      updatedAt: new Date().toISOString(),
    })
    return result.message
  }

  /** Scheduled-task callback (see POST /schedules). */
  async runScheduledPrompt(
    payload: { prompt: string; model?: string },
    schedule?: { id: string },
  ): Promise<void> {
    let ok = true
    let output = ''
    try {
      const reply = await this.runChat(
        [...this.state.messages, { role: 'user', content: payload.prompt }],
        payload.model,
      )
      output = reply.content ?? ''
    } catch (e) {
      ok = false
      output = e instanceof Error ? e.message : 'failed'
    }
    const entry: TaskResult = {
      scheduleId: schedule?.id ?? '',
      at: new Date().toISOString(),
      ok,
      output: output.slice(0, 8192),
    }
    this.setState({
      ...this.state,
      taskResults: [...this.state.taskResults, entry].slice(-MAX_TASK_RESULTS),
    })
  }

  async onRequest(request: Request): Promise<Response> {
    const url = new URL(request.url)
    const parts = url.pathname.split('/').filter(Boolean)
    const route = `${request.method} /${parts[0] ?? ''}`
    switch (route) {
      case 'GET /':
      case 'GET /state':
        return json({
          workspace: this.env.KO_WORKSPACE_ID ?? null,
          messages: this.state.messages.length,
          taskResults: this.state.taskResults,
          updatedAt: this.state.updatedAt,
          mcpServers: Object.keys(this.getMcpServers().servers ?? {}),
        })
      case 'POST /chat': {
        const body = await readJson(request)
        if (!body) return json({ error: 'invalid JSON body' }, 400)
        const model = typeof body.model === 'string' ? body.model : undefined
        let messages: ChatMessage[]
        if (typeof body.message === 'string' && body.message) {
          messages = [
            ...this.state.messages,
            { role: 'user', content: body.message },
          ]
        } else if (Array.isArray(body.messages)) {
          messages = body.messages as ChatMessage[]
        } else {
          return json({ error: 'message or messages is required' }, 400)
        }
        try {
          const reply = await this.runChat(messages, model)
          return json({ message: reply })
        } catch (e) {
          return json(
            { error: e instanceof Error ? e.message : 'chat failed' },
            502,
          )
        }
      }
      case 'DELETE /chat':
        this.setState({
          ...this.state,
          messages: [],
          updatedAt: new Date().toISOString(),
        })
        return json({ ok: true })
      case 'GET /schedules':
        return json({ schedules: await this.listSchedules() })
      case 'POST /schedules': {
        const body = await readJson(request)
        const when = body?.when
        const prompt = body?.prompt
        if (
          typeof prompt !== 'string'
          || !prompt
          || (typeof when !== 'string' && typeof when !== 'number')
        ) {
          return json(
            {
              error: 'prompt and when (seconds, ISO date or cron) are required',
            },
            400,
          )
        }
        const at =
          typeof when === 'string' && !Number.isNaN(Date.parse(when))
            ? new Date(when)
            : when
        const s = await this.schedule(at, 'runScheduledPrompt', {
          prompt,
          model: body?.model,
        })
        return json({ schedule: s }, 201)
      }
      case 'DELETE /schedules': {
        const id = parts[1]
        if (!id) return json({ error: 'schedule id required' }, 400)
        return json({ cancelled: await this.cancelSchedule(id) })
      }
      case 'GET /mcp':
        return json(this.getMcpServers())
      case 'POST /mcp': {
        const body = await readJson(request)
        if (
          !body
          || typeof body.name !== 'string'
          || !/^[A-Za-z0-9_-]{1,64}$/.test(body.name)
          || !isHttpsUrl(body.url)
        ) {
          return json({ error: 'name and an https url are required' }, 400)
        }
        const headers =
          body.headers && typeof body.headers === 'object'
            ? (body.headers as Record<string, string>)
            : undefined
        const r = await this.addMcpServer(body.name, body.url, {
          transport: { headers },
        })
        return json(r, 201)
      }
      case 'DELETE /mcp': {
        const id = parts[1]
        if (!id) return json({ error: 'server id required' }, 400)
        await this.removeMcpServer(id)
        return json({ ok: true })
      }
      default:
        return json({ error: 'not found' }, 404)
    }
  }
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const g = await gate(request, env)
    if (!g.ok) return g.response
    const workspace = env.KO_WORKSPACE_ID
    if (!workspace)
      return json({ error: 'KO_WORKSPACE_ID is not configured' }, 500)
    const agent = await getAgentByName(env.KoAgent, workspace)
    return agent.fetch(g.request)
  },
} satisfies ExportedHandler<Env>
