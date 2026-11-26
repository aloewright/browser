// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

import { loadTimeData } from '$web-common/loadTimeData'
import * as mojom from 'gen/brave/components/ko/workspace/common/ko_workspace.mojom.m.js'

export { mojom }

// Browser side: containers, tabs, captures, workspace apps.
export const pageHandler: mojom.PageHandlerRemote =
  mojom.PageHandler.getRemote()

export type TokenStatus = 'ok' | 'missing' | 'insecure' | 'invalid'

// Set by C++ through /agentd.js on every load. Never persisted by the page.
export function getAgentdSettings() {
  return {
    baseUrl: loadTimeData.getString('agentdBaseUrl'),
    token: loadTimeData.getString('agentdToken'),
    tokenStatus: loadTimeData.getString('agentdTokenStatus') as TokenStatus,
    cspPort: loadTimeData.getInteger('agentdCspPort'),
  }
}

// The CSP is built before agentd.port is read. If agentd moved, reload once
// so the CSP allows the new port.
export function reloadIfAgentdPortMoved(): boolean {
  const { baseUrl, cspPort } = getAgentdSettings()
  const key = 'koWorkspacePortReload'
  const port = baseUrl ? Number(new URL(baseUrl).port) : 0
  if (port && port !== cspPort && !sessionStorage.getItem(key)) {
    sessionStorage.setItem(key, '1')
    location.reload()
    return true
  }
  sessionStorage.removeItem(key)
  return false
}

// ko-agentd API types (ko/docs/agentd.md).
export interface CloudflareAgent {
  workspace_id?: string
  script_name?: string
  url?: string
  deployed_at?: string
}

export interface Workspace {
  id: string
  name: string
  container_id: string | null
  tags?: string[]
  default_model?: string | null
  mcp_servers?: string[]
  cloudflare_agent?: CloudflareAgent | null
  created_at?: string
  updated_at?: string
}

export interface Artifact {
  id: string
  name: string
  mime_type?: string
  size?: number
  tags?: string[]
  created_at?: string
}

export interface ArtifactWithContent extends Artifact {
  content_base64?: string
}

export interface Model {
  id: string
}

export interface McpServer {
  name: string
  type?: string
}

export interface Run {
  id: string
  status?: string
  model?: string
}

export interface RunEvent {
  seq: number
  ts?: string
  type: string
  data: unknown
}

export class AgentdError extends Error {
  constructor(
    readonly status: number,
    message: string,
  ) {
    super(message)
  }
}

function segment(id: string) {
  return encodeURIComponent(id)
}

export function bytesToBase64(bytes: Uint8Array): string {
  let binary = ''
  const chunk = 0x8000
  for (let i = 0; i < bytes.length; i += chunk) {
    binary += String.fromCharCode(...bytes.subarray(i, i + chunk))
  }
  return btoa(binary)
}

export function base64ToBytes(base64: string): Uint8Array<ArrayBuffer> {
  const binary = atob(base64)
  const bytes = new Uint8Array(binary.length)
  for (let i = 0; i < binary.length; i++) {
    bytes[i] = binary.charCodeAt(i)
  }
  return bytes
}

export class Agentd {
  constructor(
    private readonly baseUrl: string,
    private readonly token: string,
  ) {}

  private init(method: string, body?: unknown, accept?: string): RequestInit {
    const headers: Record<string, string> = {
      // The token only ever travels in this header, never in a URL.
      'Authorization': `Bearer ${this.token}`,
    }
    if (body !== undefined) {
      headers['Content-Type'] = 'application/json'
    }
    if (accept) {
      headers['Accept'] = accept
    }
    return {
      method,
      headers,
      body: body === undefined ? undefined : JSON.stringify(body),
      cache: 'no-store',
      credentials: 'omit',
      redirect: 'error',
      referrerPolicy: 'no-referrer',
    }
  }

  private async request<T>(
    method: string,
    path: string,
    body?: unknown,
  ): Promise<T> {
    const response = await fetch(this.baseUrl + path, this.init(method, body))
    if (!response.ok) {
      let message = response.statusText
      try {
        const json = await response.json()
        message = json?.error?.message ?? json?.error ?? message
      } catch {
        // Keep the status text.
      }
      throw new AgentdError(response.status, String(message))
    }
    if (response.status === 204) {
      return undefined as T
    }
    return (await response.json()) as T
  }

  async isHealthy(): Promise<boolean> {
    try {
      const response = await fetch(this.baseUrl + '/v1/health', {
        cache: 'no-store',
        credentials: 'omit',
        redirect: 'error',
      })
      return response.ok
    } catch {
      return false
    }
  }

  async listWorkspaces(): Promise<Workspace[]> {
    return (await this.request<{ data: Workspace[] }>('GET', '/v1/workspaces'))
      .data
  }

  createWorkspace(name: string, containerId: string | null) {
    return this.request<Workspace>('POST', '/v1/workspaces', {
      name,
      container_id: containerId,
    })
  }

  updateWorkspace(id: string, patch: Partial<Workspace>) {
    return this.request<Workspace>(
      'PATCH',
      `/v1/workspaces/${segment(id)}`,
      patch,
    )
  }

  getWorkspace(id: string) {
    return this.request<Workspace>('GET', `/v1/workspaces/${segment(id)}`)
  }

  deleteWorkspace(id: string) {
    return this.request<void>('DELETE', `/v1/workspaces/${segment(id)}`)
  }

  async listArtifacts(workspaceId: string): Promise<Artifact[]> {
    return (
      await this.request<{ data: Artifact[] }>(
        'GET',
        `/v1/workspaces/${segment(workspaceId)}/artifacts`,
      )
    ).data
  }

  uploadArtifact(
    workspaceId: string,
    name: string,
    mimeType: string,
    bytes: Uint8Array,
  ) {
    return this.request<Artifact>(
      'POST',
      `/v1/workspaces/${segment(workspaceId)}/artifacts`,
      {
        name,
        mime_type: mimeType || 'application/octet-stream',
        content_base64: bytesToBase64(bytes),
      },
    )
  }

  getArtifact(workspaceId: string, artifactId: string) {
    return this.request<ArtifactWithContent>(
      'GET',
      `/v1/workspaces/${segment(workspaceId)}/artifacts/${segment(artifactId)}`,
    )
  }

  deleteArtifact(workspaceId: string, artifactId: string) {
    return this.request<void>(
      'DELETE',
      `/v1/workspaces/${segment(workspaceId)}/artifacts/${segment(artifactId)}`,
    )
  }

  async listModels(): Promise<Model[]> {
    return (await this.request<{ data: Model[] }>('GET', '/v1/models')).data
  }

  async listMcpServers(): Promise<McpServer[]> {
    return (await this.request<{ data: McpServer[] }>('GET', '/v1/mcp/servers'))
      .data
  }

  startRun(
    workspaceId: string,
    model: string,
    prompt: string,
    mcpServers: string[],
  ) {
    return this.request<Run>(
      'POST',
      `/v1/workspaces/${segment(workspaceId)}/runs`,
      { model, prompt, mcp_servers: mcpServers },
    )
  }

  cancelRun(runId: string) {
    return this.request<void>('POST', `/v1/runs/${segment(runId)}/cancel`, {})
  }

  deployCloudflareAgent(workspaceId: string) {
    return this.request<CloudflareAgent>('POST', '/v1/cloudflare/agents', {
      workspace_id: workspaceId,
    })
  }

  // EventSource can't send Authorization, so read the SSE stream by hand.
  async streamRunEvents(
    runId: string,
    onEvent: (event: RunEvent) => void,
    signal: AbortSignal,
  ): Promise<void> {
    const init = this.init('GET', undefined, 'text/event-stream')
    const response = await fetch(
      `${this.baseUrl}/v1/runs/${segment(runId)}/events`,
      { ...init, signal },
    )
    if (!response.ok || !response.body) {
      throw new AgentdError(response.status, response.statusText)
    }
    const reader = response.body.pipeThrough(new TextDecoderStream()).getReader()
    let buffer = ''
    for (;;) {
      const { value, done } = await reader.read()
      if (done) {
        return
      }
      buffer += value.replace(/\r\n/g, '\n')
      let end
      while ((end = buffer.indexOf('\n\n')) >= 0) {
        const block = buffer.slice(0, end)
        buffer = buffer.slice(end + 2)
        const data = block
          .split('\n')
          .filter((line) => line.startsWith('data:'))
          .map((line) => line.slice(5).trimStart())
          .join('\n')
        if (!data) {
          continue
        }
        try {
          const event = JSON.parse(data) as RunEvent
          onEvent(event)
          if (event.type === 'done') {
            await reader.cancel()
            return
          }
        } catch {
          // Ignore malformed events.
        }
      }
    }
  }
}

let agentd: Agentd | undefined

export function getAgentd(): Agentd {
  if (!agentd) {
    const { baseUrl, token } = getAgentdSettings()
    agentd = new Agentd(baseUrl, token)
  }
  return agentd
}

export function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error)
}
