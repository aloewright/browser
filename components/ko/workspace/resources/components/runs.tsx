// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

import * as React from 'react'
import Alert from '@brave/leo/react/alert'
import Button from '@brave/leo/react/button'
import Checkbox from '@brave/leo/react/checkbox'
import Dropdown from '@brave/leo/react/dropdown'
import TextArea from '@brave/leo/react/textarea'
import { getLocale } from '$web-common/locale'
import {
  errorMessage,
  getAgentd,
  McpServer,
  Model,
  RunEvent,
  Workspace,
} from '../api'

const kMaxLines = 2000

// Providers exposed by ko-agentd, in picker order.
const kProviderOrder = ['claude-code/', 'codex/', 'cf-gateway/', 'nanogpt/']

function providerRank(id: string) {
  const index = kProviderOrder.findIndex((prefix) => id.startsWith(prefix))
  return index < 0 ? kProviderOrder.length : index
}

function describe(event: RunEvent): string {
  const data = event.data as Record<string, unknown> | string | undefined
  if (typeof data === 'string') {
    return data
  }
  switch (event.type) {
    case 'text':
      return String(data?.['text'] ?? '')
    case 'tool_use':
      return `[${String(data?.['name'] ?? 'tool')}] ${JSON.stringify(
        data?.['input'] ?? {},
      )}`
    case 'status':
      return `status: ${String(data?.['status'] ?? JSON.stringify(data))}`
    default:
      return `${event.type}: ${JSON.stringify(data ?? {})}`
  }
}

export function Runs(props: { workspace: Workspace }) {
  const [models, setModels] = React.useState<Model[]>([])
  const [servers, setServers] = React.useState<McpServer[]>([])
  const [model, setModel] = React.useState('')
  const [selectedServers, setSelectedServers] = React.useState<string[]>([])
  const [prompt, setPrompt] = React.useState('')
  const [runId, setRunId] = React.useState<string>()
  const [lines, setLines] = React.useState<string[]>([])
  const [running, setRunning] = React.useState(false)
  const [error, setError] = React.useState<string>()
  const abort = React.useRef<AbortController | undefined>(undefined)

  React.useEffect(() => {
    const agentd = getAgentd()
    agentd
      .listModels()
      .then((list) => {
        const sorted = [...list].sort(
          (a, b) =>
            providerRank(a.id) - providerRank(b.id) || a.id.localeCompare(b.id),
        )
        setModels(sorted)
        setModel(props.workspace.default_model ?? sorted[0]?.id ?? '')
      })
      .catch((e) => setError(errorMessage(e)))
    agentd
      .listMcpServers()
      .then(setServers)
      .catch(() => setServers([]))
    setSelectedServers(props.workspace.mcp_servers ?? [])
    return () => abort.current?.abort()
  }, [props.workspace.id])

  const start = async () => {
    setError(undefined)
    setLines([])
    setRunning(true)
    abort.current?.abort()
    abort.current = new AbortController()
    try {
      const run = await getAgentd().startRun(
        props.workspace.id,
        model,
        prompt,
        selectedServers,
      )
      setRunId(run.id)
      await getAgentd().streamRunEvents(
        run.id,
        (event) =>
          setLines((current) => [...current, describe(event)].slice(-kMaxLines)),
        abort.current.signal,
      )
    } catch (e) {
      if (!(e instanceof DOMException && e.name === 'AbortError')) {
        setError(errorMessage(e))
      }
    } finally {
      setRunning(false)
    }
  }

  const cancel = async () => {
    if (runId) {
      try {
        await getAgentd().cancelRun(runId)
      } catch (e) {
        setError(errorMessage(e))
      }
    }
  }

  const toggleServer = (name: string, checked: boolean) => {
    setSelectedServers((current) =>
      checked ? [...current, name] : current.filter((n) => n !== name),
    )
  }

  return (
    <section className='card'>
      <h2>{getLocale(S.KO_WORKSPACE_RUNS)}</h2>
      <Dropdown
        value={model}
        onChange={({ value }) => setModel(String(value))}
      >
        <div slot='label'>{getLocale(S.KO_WORKSPACE_MODEL)}</div>
        <span slot='value'>{model}</span>
        {models.map((m) => (
          <leo-option
            key={m.id}
            value={m.id}
          >
            {m.id}
          </leo-option>
        ))}
      </Dropdown>
      {servers.length > 0 && (
        <fieldset className='servers'>
          <legend>{getLocale(S.KO_WORKSPACE_MCP_SERVERS)}</legend>
          {servers.map((server) => (
            <Checkbox
              key={server.name}
              checked={selectedServers.includes(server.name)}
              onChange={({ checked }) => toggleServer(server.name, checked)}
            >
              {server.name}
            </Checkbox>
          ))}
        </fieldset>
      )}
      <TextArea
        value={prompt}
        placeholder={getLocale(S.KO_WORKSPACE_PROMPT_PLACEHOLDER)}
        onInput={(detail) => setPrompt(detail.value)}
      />
      <div className='row'>
        <Button
          isDisabled={!model || !prompt.trim() || running}
          isLoading={running}
          onClick={start}
        >
          {getLocale(S.KO_WORKSPACE_START_RUN)}
        </Button>
        {running && (
          <Button
            kind='outline'
            onClick={cancel}
          >
            {getLocale(S.KO_WORKSPACE_CANCEL_RUN)}
          </Button>
        )}
      </div>
      {error && <Alert type='error'>{error}</Alert>}
      {lines.length > 0 && (
        <div className='run-output'>
          <h3 className='muted small'>{getLocale(S.KO_WORKSPACE_RUN_OUTPUT)}</h3>
          <pre>{lines.join('\n')}</pre>
        </div>
      )}
    </section>
  )
}
