// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

import * as React from 'react'
import Alert from '@brave/leo/react/alert'
import Button from '@brave/leo/react/button'
import TextArea from '@brave/leo/react/textarea'
import { getLocale } from '$web-common/locale'
import { errorMessage, getAgentd, mojom, pageHandler, Workspace } from '../api'
import { Artifacts } from './artifacts'
import { Runs } from './runs'

// Tab lists are a local convenience only; they are not secret.
function tabsKey(workspaceId: string) {
  return `koWorkspaceTabs:${workspaceId}`
}

function loadTabs(workspaceId: string): string {
  try {
    return localStorage.getItem(tabsKey(workspaceId)) ?? ''
  } catch {
    return ''
  }
}

function saveTabs(workspaceId: string, value: string) {
  try {
    localStorage.setItem(tabsKey(workspaceId), value)
  } catch {
    // Storage unavailable; the list stays in memory.
  }
}

function parseUrls(text: string): mojom.WorkspaceApp['url'][] {
  return text
    .split('\n')
    .map((line) => line.trim())
    .filter((line) => {
      try {
        const url = new URL(line)
        return url.protocol === 'https:' || url.protocol === 'http:'
      } catch {
        return false
      }
    })
    .map((url) => ({ url }))
}

export function WorkspaceDetail(props: {
  workspace: Workspace
  containers: mojom.WorkspaceContainer[]
  onBack: () => void
  onChanged: () => void
}) {
  const { workspace } = props
  const [tabs, setTabs] = React.useState(() => loadTabs(workspace.id))
  const [error, setError] = React.useState<string>()
  const [deploying, setDeploying] = React.useState(false)
  const container = props.containers.find(
    (c) => c.id === workspace.container_id,
  )

  const linkContainer = async () => {
    try {
      const { container: created } = await pageHandler.createContainer(
        workspace.name,
      )
      if (created) {
        await getAgentd().updateWorkspace(workspace.id, {
          container_id: created.id,
        })
        props.onChanged()
      }
    } catch (e) {
      setError(errorMessage(e))
    }
  }

  const openTabs = async () => {
    if (!container) {
      return
    }
    saveTabs(workspace.id, tabs)
    await pageHandler.openTabsInContainer(container.id, parseUrls(tabs))
  }

  const deploy = async () => {
    setDeploying(true)
    setError(undefined)
    try {
      await getAgentd().deployCloudflareAgent(workspace.id)
      props.onChanged()
    } catch (e) {
      setError(errorMessage(e))
    } finally {
      setDeploying(false)
    }
  }

  const remove = async () => {
    try {
      await getAgentd().deleteWorkspace(workspace.id)
      props.onBack()
      props.onChanged()
    } catch (e) {
      setError(errorMessage(e))
    }
  }

  const agentUrl = workspace.cloudflare_agent?.url
  return (
    <>
      <div className='section-header'>
        <Button
          kind='plain-faint'
          size='small'
          onClick={props.onBack}
        >
          {getLocale(S.KO_WORKSPACE_BACK)}
        </Button>
        <Button
          kind='plain-faint'
          size='small'
          onClick={remove}
        >
          {getLocale(S.KO_WORKSPACE_DELETE_WORKSPACE)}
        </Button>
      </div>
      <h2>{workspace.name}</h2>
      {error && (
        <Alert type='error'>
          {getLocale(S.KO_WORKSPACE_ERROR)} {error}
        </Alert>
      )}
      <section className='card'>
        <div className='section-header'>
          <span>
            {getLocale(S.KO_WORKSPACE_CONTAINER_LABEL)}:{' '}
            {container?.name ?? getLocale(S.KO_WORKSPACE_CONTAINER_MISSING)}
          </span>
          {!container && (
            <Button
              kind='outline'
              size='small'
              onClick={linkContainer}
            >
              {getLocale(S.KO_WORKSPACE_CREATE)}
            </Button>
          )}
        </div>
        <TextArea
          value={tabs}
          placeholder={getLocale(S.KO_WORKSPACE_URLS_PLACEHOLDER)}
          onInput={(detail) => setTabs(detail.value)}
        />
        <Button
          kind='outline'
          isDisabled={!container || parseUrls(tabs).length === 0}
          onClick={openTabs}
        >
          {getLocale(S.KO_WORKSPACE_OPEN_TABS)}
        </Button>
      </section>
      <Artifacts workspaceId={workspace.id} />
      <Runs workspace={workspace} />
      <section className='card'>
        <h2>{getLocale(S.KO_WORKSPACE_CLOUDFLARE)}</h2>
        <p className='muted'>
          {agentUrl ?? getLocale(S.KO_WORKSPACE_NOT_DEPLOYED)}
        </p>
        <Button
          kind='outline'
          isLoading={deploying}
          onClick={deploy}
        >
          {getLocale(S.KO_WORKSPACE_DEPLOY)}
        </Button>
      </section>
    </>
  )
}
