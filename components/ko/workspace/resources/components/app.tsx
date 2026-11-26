// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

import * as React from 'react'
import Alert from '@brave/leo/react/alert'
import Button from '@brave/leo/react/button'
import Input from '@brave/leo/react/input'
import ProgressRing from '@brave/leo/react/progressRing'
import { getLocale } from '$web-common/locale'
import {
  errorMessage,
  getAgentd,
  getAgentdSettings,
  mojom,
  pageHandler,
  Workspace,
} from '../api'
import { WorkspaceApps } from './apps'
import { WorkspaceDetail } from './workspace_detail'

function tokenProblem(): string | undefined {
  switch (getAgentdSettings().tokenStatus) {
    case 'ok':
      return undefined
    case 'insecure':
      return getLocale(S.KO_WORKSPACE_TOKEN_INSECURE)
    case 'invalid':
      return getLocale(S.KO_WORKSPACE_TOKEN_INVALID)
    default:
      return getLocale(S.KO_WORKSPACE_TOKEN_MISSING)
  }
}

export function App() {
  const [workspaces, setWorkspaces] = React.useState<Workspace[]>()
  const [containers, setContainers] = React.useState<
    mojom.WorkspaceContainer[]
  >([])
  const [selectedId, setSelectedId] = React.useState<string>()
  const [newName, setNewName] = React.useState('')
  const [creating, setCreating] = React.useState(false)
  const [error, setError] = React.useState<string>()
  const [offline, setOffline] = React.useState(false)
  const problem = tokenProblem()

  const refresh = React.useCallback(async () => {
    setContainers((await pageHandler.getContainers()).containers)
    try {
      setWorkspaces(await getAgentd().listWorkspaces())
      setOffline(false)
    } catch (e) {
      if (!(await getAgentd().isHealthy())) {
        setOffline(true)
      } else {
        setError(errorMessage(e))
      }
      setWorkspaces([])
    }
  }, [])

  React.useEffect(() => {
    if (!problem) {
      refresh()
    }
  }, [problem, refresh])

  const create = async () => {
    const name = newName.trim()
    if (!name) {
      return
    }
    setCreating(true)
    setError(undefined)
    try {
      // Each workspace gets its own Brave Container for its tabs.
      const { container } = await pageHandler.createContainer(name)
      const workspace = await getAgentd().createWorkspace(
        name,
        container?.id ?? null,
      )
      setNewName('')
      await refresh()
      setSelectedId(workspace.id)
    } catch (e) {
      setError(errorMessage(e))
    } finally {
      setCreating(false)
    }
  }

  const header = (
    <header className='page-header'>
      <h1>{getLocale(S.KO_WORKSPACE_TITLE)}</h1>
      <div className='row'>
        <Button
          kind='plain-faint'
          size='small'
          onClick={() => pageHandler.showWorkspace(true)}
        >
          {getLocale(S.KO_WORKSPACE_SHOW_IN_SIDE_PANEL)}
        </Button>
        <Button
          kind='plain-faint'
          size='small'
          onClick={() => pageHandler.showWorkspace(false)}
        >
          {getLocale(S.KO_WORKSPACE_SHOW_IN_TAB)}
        </Button>
      </div>
    </header>
  )

  if (problem || offline) {
    return (
      <main className='page'>
        {header}
        <Alert type='warning'>
          {problem ?? getLocale(S.KO_WORKSPACE_AGENTD_OFFLINE)}
          <div slot='actions'>
            <Button
              kind='outline'
              size='small'
              onClick={() => location.reload()}
            >
              {getLocale(S.KO_WORKSPACE_RELOAD)}
            </Button>
          </div>
        </Alert>
        <WorkspaceApps />
      </main>
    )
  }

  const selected = workspaces?.find((w) => w.id === selectedId)
  if (selected) {
    return (
      <main className='page'>
        {header}
        <WorkspaceDetail
          workspace={selected}
          containers={containers}
          onBack={() => setSelectedId(undefined)}
          onChanged={refresh}
        />
      </main>
    )
  }

  return (
    <main className='page'>
      {header}
      {error && (
        <Alert type='error'>
          {getLocale(S.KO_WORKSPACE_ERROR)} {error}
        </Alert>
      )}
      <section className='card'>
        <h2>{getLocale(S.KO_WORKSPACE_NEW)}</h2>
        <div className='row'>
          <Input
            className='grow'
            value={newName}
            maxlength={64}
            placeholder={getLocale(S.KO_WORKSPACE_NAME_PLACEHOLDER)}
            onInput={(detail) => setNewName(detail.value)}
            onKeyDown={(e) => {
              if (e.innerEvent.key === 'Enter') {
                create()
              }
            }}
          />
          <Button
            isDisabled={!newName.trim()}
            isLoading={creating}
            onClick={create}
          >
            {getLocale(S.KO_WORKSPACE_CREATE)}
          </Button>
        </div>
      </section>
      <section className='card'>
        {workspaces === undefined ? (
          <ProgressRing />
        ) : workspaces.length === 0 ? (
          <p className='muted'>{getLocale(S.KO_WORKSPACE_EMPTY)}</p>
        ) : (
          <ul className='list'>
            {workspaces.map((workspace) => (
              <li key={workspace.id}>
                <Button
                  kind='plain'
                  className='list-item'
                  onClick={() => setSelectedId(workspace.id)}
                >
                  {workspace.name}
                </Button>
              </li>
            ))}
          </ul>
        )}
      </section>
      <WorkspaceApps />
    </main>
  )
}
