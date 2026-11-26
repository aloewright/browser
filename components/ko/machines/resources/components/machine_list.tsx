// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

import * as React from 'react'
import Alert from '@brave/leo/react/alert'
import Button from '@brave/leo/react/button'
import Dialog from '@brave/leo/react/dialog'
import Icon from '@brave/leo/react/icon'
import { formatLocale, getLocale } from '$web-common/locale'
import {
  ImportStatus,
  Machine,
  MachineKind,
  MachinesState,
  getHandler,
  listenForChanges,
  openDesktop,
} from '../api'
import { MachineEditor } from './machine_editor'
import '../strings'

type Notice = { type: 'info' | 'error'; text: string }

type EditorState = { machine: Machine | null } | null

function kindLabel(kind: number) {
  return kind === MachineKind.Vnc
    ? getLocale(S.KO_MACHINES_KIND_VNC)
    : getLocale(S.KO_MACHINES_KIND_SCREEN_SHARING)
}

function importNotice(
  status: number,
  added: number,
  allowlistUpdated: boolean,
): Notice {
  if (status === ImportStatus.NotFound) {
    return { type: 'error', text: getLocale(S.KO_MACHINES_IMPORT_NOT_FOUND) }
  }
  if (status === ImportStatus.InsecureFile) {
    return { type: 'error', text: getLocale(S.KO_MACHINES_IMPORT_INSECURE) }
  }
  if (status === ImportStatus.InvalidFile) {
    return { type: 'error', text: getLocale(S.KO_MACHINES_IMPORT_INVALID) }
  }
  let text = formatLocale(S.KO_MACHINES_IMPORT_DONE, {
    $1: String(added),
  }) as string
  if (allowlistUpdated) {
    text += ' ' + getLocale(S.KO_MACHINES_IMPORT_NETWORKS_UPDATED)
  }
  return { type: 'info', text }
}

function MachineRow(props: {
  machine: Machine
  onEdit: () => void
  onRemove: () => void
}) {
  const { machine } = props
  return (
    <li className='machine'>
      <Icon name='smartphone-desktop' />
      <div className='machine-text'>
        <span className='machine-name'>{machine.name}</span>
        <span className='machine-detail'>
          {`${machine.host}:${machine.port} · ${kindLabel(machine.kind)}`}
        </span>
        {machine.hasCredentials && (
          <span className='saved'>
            <Icon name='key' />
            {getLocale(S.KO_MACHINES_PASSWORD_SAVED)}
          </span>
        )}
      </div>
      <div className='actions'>
        <Button
          kind='plain-faint'
          fab
          title={getLocale(S.KO_MACHINES_EDIT)}
          aria-label={getLocale(S.KO_MACHINES_EDIT)}
          onClick={props.onEdit}
        >
          <Icon name='edit-pencil' />
        </Button>
        <Button
          kind='plain-faint'
          fab
          title={getLocale(S.KO_MACHINES_REMOVE)}
          aria-label={getLocale(S.KO_MACHINES_REMOVE)}
          onClick={props.onRemove}
        >
          <Icon name='trash' />
        </Button>
        <Button onClick={() => openDesktop(machine.id)}>
          <div slot='icon-before'>
            <Icon name='launch' />
          </div>
          {getLocale(S.KO_MACHINES_OPEN_DESKTOP)}
        </Button>
      </div>
    </li>
  )
}

export function MachineList() {
  const [state, setState] = React.useState<MachinesState | null>(null)
  const [editor, setEditor] = React.useState<EditorState>(null)
  const [removeTarget, setRemoveTarget] = React.useState<Machine | null>(null)
  const [notice, setNotice] = React.useState<Notice | null>(null)
  const [importing, setImporting] = React.useState(false)

  const refresh = React.useCallback(async () => {
    setState(await getHandler().getMachines())
  }, [])

  React.useEffect(() => {
    listenForChanges(refresh)
    refresh()
  }, [refresh])

  const onImport = async () => {
    setImporting(true)
    try {
      const { status, added, allowlistUpdated } =
        await getHandler().importFromFile()
      setNotice(importNotice(status, added, allowlistUpdated))
    } finally {
      setImporting(false)
    }
  }

  const onConfirmRemove = async () => {
    if (removeTarget) {
      await getHandler().removeMachine(removeTarget.id)
    }
    setRemoveTarget(null)
  }

  return (
    <main className='page'>
      <header className='page-header'>
        <div className='text'>
          <h1>{getLocale(S.KO_MACHINES_TITLE)}</h1>
          <p>{getLocale(S.KO_MACHINES_DESCRIPTION)}</p>
        </div>
        <div className='actions'>
          <Button
            kind='outline'
            isLoading={importing}
            onClick={onImport}
          >
            <div slot='icon-before'>
              <Icon name='import-arrow' />
            </div>
            {getLocale(S.KO_MACHINES_IMPORT)}
          </Button>
          <Button onClick={() => setEditor({ machine: null })}>
            <div slot='icon-before'>
              <Icon name='plus-add' />
            </div>
            {getLocale(S.KO_MACHINES_ADD)}
          </Button>
        </div>
      </header>

      {notice && (
        <Alert type={notice.type === 'error' ? 'error' : 'info'}>
          {notice.text}
        </Alert>
      )}

      {state
        && (state.machines.length === 0 ? (
          <p className='empty'>{getLocale(S.KO_MACHINES_EMPTY)}</p>
        ) : (
          <ul className='machines'>
            {state.machines.map((machine) => (
              <MachineRow
                key={machine.id}
                machine={machine}
                onEdit={() => setEditor({ machine })}
                onRemove={() => setRemoveTarget(machine)}
              />
            ))}
          </ul>
        ))}

      {state && (
        <section className='panel'>
          <h2>{getLocale(S.KO_MACHINES_ALLOWED_NETWORKS)}</h2>
          {state.allowedCidrs.length > 0 ? (
            <ul className='cidrs'>
              {state.allowedCidrs.map((cidr) => (
                <li key={cidr}>{cidr}</li>
              ))}
            </ul>
          ) : (
            <Alert type='warning'>
              {getLocale(S.KO_MACHINES_NO_ALLOWED_NETWORKS)}
            </Alert>
          )}
          {state.rejectedCidrs.length > 0 && (
            <p className='help'>
              {formatLocale(S.KO_MACHINES_IGNORED_NETWORKS, {
                $1: state.rejectedCidrs.join(', '),
              })}
            </p>
          )}
          <p className='help'>
            {getLocale(S.KO_MACHINES_ALLOWED_NETWORKS_HELP)}
          </p>
        </section>
      )}

      {editor && (
        <MachineEditor
          machine={editor.machine}
          onClose={() => setEditor(null)}
        />
      )}

      <Dialog
        isOpen={!!removeTarget}
        onClose={() => setRemoveTarget(null)}
      >
        <div slot='title'>
          {removeTarget
            && formatLocale(S.KO_MACHINES_REMOVE_TITLE, {
              $1: removeTarget.name,
            })}
        </div>
        <p>{getLocale(S.KO_MACHINES_REMOVE_BODY)}</p>
        <div
          slot='actions'
          className='dialog-actions'
        >
          <Button
            kind='outline'
            onClick={() => setRemoveTarget(null)}
          >
            {getLocale(S.KO_MACHINES_CANCEL)}
          </Button>
          <Button onClick={onConfirmRemove}>
            {getLocale(S.KO_MACHINES_REMOVE_CONFIRM)}
          </Button>
        </div>
      </Dialog>
    </main>
  )
}
