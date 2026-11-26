// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

import * as React from 'react'
import Alert from '@brave/leo/react/alert'
import Button from '@brave/leo/react/button'
import Icon from '@brave/leo/react/icon'
import ProgressRing from '@brave/leo/react/progressRing'
import Toggle from '@brave/leo/react/toggle'
import RFB from '../../../../../third_party/novnc/core/rfb.js'
import { formatLocale, getLocale } from '$web-common/locale'
import { ConnectResult, Machine, connectToMachine, getHandler } from '../api'
import { CredentialsDialog, EnteredCredentials } from './credentials_dialog'
import '../strings'

type Status = 'loading' | 'connecting' | 'connected' | 'disconnected'

interface Prompt {
  types: string[]
  error?: string
}

function connectError(result: number, host: string): string {
  switch (result) {
    case ConnectResult.UnknownMachine:
      return getLocale(S.KO_MACHINES_ERROR_UNKNOWN_MACHINE)
    case ConnectResult.NoAllowlist:
      return getLocale(S.KO_MACHINES_ERROR_NO_ALLOWLIST)
    case ConnectResult.NotAllowed:
      return formatLocale(S.KO_MACHINES_ERROR_NOT_ALLOWED, { $1: host })
    case ConnectResult.ResolveFailed:
      return formatLocale(S.KO_MACHINES_ERROR_RESOLVE, { $1: host })
    case ConnectResult.TooManyConnections:
      return getLocale(S.KO_MACHINES_ERROR_TOO_MANY_CONNECTIONS)
    default:
      return formatLocale(S.KO_MACHINES_ERROR_CONNECT, { $1: host })
  }
}

function statusText(status: Status) {
  switch (status) {
    case 'connected':
      return getLocale(S.KO_MACHINES_CONNECTED)
    case 'disconnected':
      return getLocale(S.KO_MACHINES_DISCONNECTED)
    default:
      return getLocale(S.KO_MACHINES_CONNECTING)
  }
}

export function Viewer(props: { machineId: string }) {
  const { machineId } = props
  const screenRef = React.useRef<HTMLDivElement>(null)
  const rfbRef = React.useRef<RFB | null>(null)
  const [machine, setMachine] = React.useState<Machine | null>(null)
  const [status, setStatus] = React.useState<Status>('loading')
  const [error, setError] = React.useState('')
  const [prompt, setPrompt] = React.useState<Prompt | null>(null)

  const [viewOnly, setViewOnly] = React.useState(false)
  const [scale, setScale] = React.useState(true)
  // Clipboard sharing is off until the person turns it on, every session.
  const [clipboard, setClipboard] = React.useState(false)
  const viewOnlyRef = React.useRef(viewOnly)
  const scaleRef = React.useRef(scale)
  const clipboardRef = React.useRef(clipboard)

  // Saved credentials are tried once per connection, and not again after the
  // server rejected a sign-in.
  const triedSaved = React.useRef(false)
  const authFailed = React.useRef(false)
  // Saved only after the server accepts them.
  const pendingSave = React.useRef<EnteredCredentials | null>(null)

  React.useEffect(() => {
    viewOnlyRef.current = viewOnly
    if (rfbRef.current) {
      rfbRef.current.viewOnly = viewOnly
    }
  }, [viewOnly])

  React.useEffect(() => {
    scaleRef.current = scale
    if (rfbRef.current) {
      rfbRef.current.scaleViewport = scale
    }
  }, [scale])

  React.useEffect(() => {
    clipboardRef.current = clipboard
  }, [clipboard])

  const onCredentialsRequired = React.useCallback(
    async (rfb: RFB, target: Machine, types: string[]) => {
      if (!triedSaved.current && !authFailed.current && target.hasCredentials) {
        triedSaved.current = true
        const { username, password } = await getHandler().getCredentials(
          target.id,
        )
        if (rfbRef.current !== rfb) {
          return
        }
        if (password && (!types.includes('username') || username)) {
          rfb.sendCredentials({ username: username ?? undefined, password })
          return
        }
      }
      setPrompt({ types })
    },
    [],
  )

  const connect = React.useCallback(
    async (target: Machine) => {
      rfbRef.current?.disconnect()
      rfbRef.current = null
      pendingSave.current = null
      triedSaved.current = false
      setError('')
      setPrompt(null)
      setStatus('connecting')

      const { result, channel } = await connectToMachine(target.id)
      if (!channel || !screenRef.current) {
        channel?.close()
        setError(connectError(result, target.host))
        setStatus('disconnected')
        return
      }

      let created: RFB | null = null
      try {
        created = new RFB(screenRef.current, channel, { shared: true })
      } catch {
        created = null
      }
      if (!created) {
        channel.close()
        setError(connectError(ConnectResult.ConnectFailed, target.host))
        setStatus('disconnected')
        return
      }
      const rfb = created
      rfbRef.current = rfb
      rfb.viewOnly = viewOnlyRef.current
      rfb.scaleViewport = scaleRef.current
      rfb.background = 'var(--leo-color-page-background)'

      rfb.addEventListener('connect', () => {
        if (rfbRef.current !== rfb) {
          return
        }
        setStatus('connected')
        authFailed.current = false
        const toSave = pendingSave.current
        pendingSave.current = null
        if (toSave?.remember) {
          getHandler()
            .setCredentials(target.id, toSave.username ?? null, toSave.password)
            .then(() =>
              setMachine((current) =>
                current ? { ...current, hasCredentials: true } : current,
              ),
            )
        }
        rfb.focus()
      })
      rfb.addEventListener('disconnect', () => {
        if (rfbRef.current === rfb) {
          rfbRef.current = null
        }
        pendingSave.current = null
        setPrompt(null)
        setStatus('disconnected')
      })
      rfb.addEventListener('credentialsrequired', (event) => {
        onCredentialsRequired(rfb, target, event.detail.types)
      })
      rfb.addEventListener('securityfailure', () => {
        authFailed.current = true
        pendingSave.current = null
        setError(getLocale(S.KO_MACHINES_AUTH_FAILED))
      })
      rfb.addEventListener('serververification', () => {
        // Fail closed rather than trust an unverified server key.
        setError(getLocale(S.KO_MACHINES_ERROR_SERVER_VERIFICATION))
        rfb.disconnect()
      })
      rfb.addEventListener('clipboard', (event) => {
        if (clipboardRef.current) {
          navigator.clipboard.writeText(event.detail.text).catch(() => {})
        }
      })
    },
    [onCredentialsRequired],
  )

  React.useEffect(() => {
    if (!machineId) {
      setError(getLocale(S.KO_MACHINES_INVALID_LINK))
      setStatus('disconnected')
      return
    }
    getHandler()
      .getMachines()
      .then(({ machines }) => {
        const found = machines.find((m) => m.id === machineId)
        if (!found) {
          setError(getLocale(S.KO_MACHINES_ERROR_UNKNOWN_MACHINE))
          setStatus('disconnected')
          return
        }
        document.title = found.name
        setMachine(found)
        connect(found)
      })
    return () => {
      rfbRef.current?.disconnect()
      rfbRef.current = null
    }
  }, [machineId, connect])

  const onPromptSubmit = (entered: EnteredCredentials) => {
    const rfb = rfbRef.current
    setPrompt(null)
    if (!rfb) {
      return
    }
    setError('')
    pendingSave.current = entered.remember ? entered : null
    rfb.sendCredentials({
      username: entered.username,
      password: entered.password,
    })
  }

  const onPromptCancel = () => {
    setPrompt(null)
    rfbRef.current?.disconnect()
  }

  const sendClipboard = async () => {
    try {
      const text = await navigator.clipboard.readText()
      rfbRef.current?.clipboardPasteFrom(text)
    } catch {
      setError(getLocale(S.KO_MACHINES_CLIPBOARD_DENIED))
    }
  }

  const goFullScreen = () => {
    screenRef.current?.parentElement?.requestFullscreen().catch(() => {})
  }

  const connected = status === 'connected'

  return (
    <div className='viewer'>
      <header className='toolbar'>
        <Button
          kind='plain-faint'
          size='small'
          href='/'
        >
          {getLocale(S.KO_MACHINES_BACK)}
        </Button>
        <div className='title'>
          <span className='machine-name'>{machine?.name ?? ''}</span>
          <span
            className='status'
            role='status'
          >
            {(status === 'connecting' || status === 'loading') && (
              <ProgressRing />
            )}
            {statusText(status)}
          </span>
        </div>
        <div className='controls'>
          <Toggle
            size='small'
            checked={viewOnly}
            onChange={({ checked }) => setViewOnly(checked)}
          >
            {getLocale(S.KO_MACHINES_VIEW_ONLY)}
          </Toggle>
          <Toggle
            size='small'
            checked={scale}
            onChange={({ checked }) => setScale(checked)}
          >
            {getLocale(S.KO_MACHINES_SCALE)}
          </Toggle>
          <Toggle
            size='small'
            checked={clipboard}
            onChange={({ checked }) => setClipboard(checked)}
          >
            {getLocale(S.KO_MACHINES_CLIPBOARD)}
          </Toggle>
          {clipboard && (
            <Button
              kind='outline'
              size='small'
              isDisabled={!connected || viewOnly}
              onClick={sendClipboard}
            >
              <div slot='icon-before'>
                <Icon name='copy' />
              </div>
              {getLocale(S.KO_MACHINES_SEND_CLIPBOARD)}
            </Button>
          )}
          <Button
            kind='plain-faint'
            size='small'
            fab
            title={getLocale(S.KO_MACHINES_FULL_SCREEN)}
            aria-label={getLocale(S.KO_MACHINES_FULL_SCREEN)}
            isDisabled={!connected}
            onClick={goFullScreen}
          >
            <Icon name='fullscreen-on' />
          </Button>
          {connected || status === 'connecting' ? (
            <Button
              kind='outline'
              size='small'
              onClick={() => rfbRef.current?.disconnect()}
            >
              {getLocale(S.KO_MACHINES_DISCONNECT)}
            </Button>
          ) : (
            <Button
              size='small'
              isDisabled={!machine}
              onClick={() => machine && connect(machine)}
            >
              <div slot='icon-before'>
                <Icon name='refresh' />
              </div>
              {getLocale(S.KO_MACHINES_RECONNECT)}
            </Button>
          )}
        </div>
      </header>
      <div className='screen'>
        <div
          ref={screenRef}
          className='rfb-target'
        />
        {error && (
          <div className='screen-overlay'>
            <Alert type='error'>{error}</Alert>
          </div>
        )}
      </div>
      {prompt && machine && (
        <CredentialsDialog
          machineName={machine.name}
          types={prompt.types}
          error={prompt.error}
          onSubmit={onPromptSubmit}
          onCancel={onPromptCancel}
        />
      )}
    </div>
  )
}
