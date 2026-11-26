// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

import * as React from 'react'
import Alert from '@brave/leo/react/alert'
import Button from '@brave/leo/react/button'
import Dialog from '@brave/leo/react/dialog'
import Dropdown from '@brave/leo/react/dropdown'
import Input from '@brave/leo/react/input'
import { getLocale } from '$web-common/locale'
import { Machine, MachineError, MachineKind, getHandler } from '../api'
import '../strings'

const DEFAULT_PORT = 5900

type Field = 'name' | 'host' | 'port'

function fieldForError(error: number): Field | null {
  if (error === MachineError.InvalidName) {
    return 'name'
  }
  if (error === MachineError.InvalidHost || error === MachineError.Duplicate) {
    return 'host'
  }
  if (error === MachineError.InvalidPort) {
    return 'port'
  }
  return null
}

export function errorText(error: number) {
  switch (error) {
    case MachineError.InvalidName:
      return getLocale(S.KO_MACHINES_ERROR_NAME)
    case MachineError.InvalidHost:
      return getLocale(S.KO_MACHINES_ERROR_HOST)
    case MachineError.InvalidPort:
      return getLocale(S.KO_MACHINES_ERROR_PORT)
    case MachineError.Duplicate:
      return getLocale(S.KO_MACHINES_ERROR_DUPLICATE)
    case MachineError.TooMany:
      return getLocale(S.KO_MACHINES_ERROR_TOO_MANY)
    case MachineError.InvalidCredentials:
      return getLocale(S.KO_MACHINES_ERROR_CREDENTIALS)
    case MachineError.EncryptionUnavailable:
      return getLocale(S.KO_MACHINES_ERROR_ENCRYPTION)
    default:
      return getLocale(S.KO_MACHINES_ERROR_NOT_FOUND)
  }
}

interface Props {
  // null to add a machine.
  machine: Machine | null
  onClose: () => void
}

export function MachineEditor(props: Props) {
  const { machine } = props
  const [name, setName] = React.useState(machine?.name ?? '')
  const [host, setHost] = React.useState(machine?.host ?? '')
  const [port, setPort] = React.useState(String(machine?.port ?? DEFAULT_PORT))
  const [kind, setKind] = React.useState(
    machine?.kind ?? MachineKind.ScreenSharing,
  )
  const [username, setUsername] = React.useState('')
  const [password, setPassword] = React.useState('')
  const [fieldError, setFieldError] = React.useState<{
    field: Field
    text: string
  } | null>(null)
  const [formError, setFormError] = React.useState('')
  const [saving, setSaving] = React.useState(false)
  const [hasSavedPassword, setHasSavedPassword] = React.useState(
    !!machine?.hasCredentials,
  )

  const save = async () => {
    setFieldError(null)
    setFormError('')
    const portNumber = Number(port)
    if (!Number.isInteger(portNumber) || portNumber < 1 || portNumber > 65535) {
      setFieldError({
        field: 'port',
        text: errorText(MachineError.InvalidPort),
      })
      return
    }
    setSaving(true)
    try {
      const input = { name, host, port: portNumber, kind }
      let id = machine?.id
      let error: number
      if (machine) {
        error = (await getHandler().updateMachine(machine.id, input)).error
      } else {
        const result = await getHandler().addMachine(input)
        error = result.error
        id = result.id ?? undefined
      }
      if (error !== MachineError.None || !id) {
        const field = fieldForError(error)
        if (field) {
          setFieldError({ field, text: errorText(error) })
        } else {
          setFormError(errorText(error))
        }
        return
      }
      if (password) {
        const usernameToSave =
          kind === MachineKind.ScreenSharing && username ? username : null
        const { error: credentialsError } = await getHandler().setCredentials(
          id,
          usernameToSave,
          password,
        )
        setPassword('')
        if (credentialsError !== MachineError.None) {
          setFormError(errorText(credentialsError))
          return
        }
      }
      props.onClose()
    } finally {
      setSaving(false)
    }
  }

  const forgetPassword = async () => {
    if (machine) {
      await getHandler().clearCredentials(machine.id)
      setHasSavedPassword(false)
    }
  }

  const errorFor = (field: Field) =>
    fieldError?.field === field ? fieldError.text : undefined

  return (
    <Dialog
      isOpen
      showClose
      backdropClickCloses={false}
      onClose={props.onClose}
    >
      <div slot='title'>
        {machine
          ? getLocale(S.KO_MACHINES_EDIT_TITLE)
          : getLocale(S.KO_MACHINES_ADD_TITLE)}
      </div>
      <div
        className='form'
        onKeyDown={(e) => {
          if (
            e.key === 'Enter'
            && (e.target as HTMLElement).matches('leo-input')
          ) {
            save()
          }
        }}
      >
        <Input
          value={name}
          required
          maxlength={128}
          showErrors={!!errorFor('name')}
          onInput={(detail) => setName(detail.value)}
        >
          {getLocale(S.KO_MACHINES_NAME_LABEL)}
          <div slot='errors'>{errorFor('name')}</div>
        </Input>
        <div className='row'>
          <div>
            <Input
              value={host}
              required
              spellcheck={false}
              showErrors={!!errorFor('host')}
              onInput={(detail) => setHost(detail.value)}
            >
              {getLocale(S.KO_MACHINES_HOST_LABEL)}
              <div slot='errors'>{errorFor('host')}</div>
            </Input>
            <p className='help'>{getLocale(S.KO_MACHINES_HOST_HELP)}</p>
          </div>
          <div className='port'>
            <Input
              type='number'
              value={port}
              showErrors={!!errorFor('port')}
              onInput={(detail) => setPort(detail.value)}
            >
              {getLocale(S.KO_MACHINES_PORT_LABEL)}
              <div slot='errors'>{errorFor('port')}</div>
            </Input>
          </div>
        </div>
        <Dropdown
          value={String(kind)}
          onChange={({ value }) => setKind(Number(value))}
        >
          <div slot='label'>{getLocale(S.KO_MACHINES_KIND_LABEL)}</div>
          <span slot='value'>
            {kind === MachineKind.Vnc
              ? getLocale(S.KO_MACHINES_KIND_VNC)
              : getLocale(S.KO_MACHINES_KIND_SCREEN_SHARING)}
          </span>
          <leo-option value={String(MachineKind.ScreenSharing)}>
            {getLocale(S.KO_MACHINES_KIND_SCREEN_SHARING)}
          </leo-option>
          <leo-option value={String(MachineKind.Vnc)}>
            {getLocale(S.KO_MACHINES_KIND_VNC)}
          </leo-option>
        </Dropdown>
        {kind === MachineKind.ScreenSharing && (
          <div>
            <Input
              value={username}
              autocomplete='off'
              spellcheck={false}
              onInput={(detail) => setUsername(detail.value)}
            >
              {getLocale(S.KO_MACHINES_USERNAME_LABEL)}
            </Input>
            <p className='help'>{getLocale(S.KO_MACHINES_USERNAME_HELP)}</p>
          </div>
        )}
        <div>
          <Input
            type='password'
            value={password}
            autocomplete='new-password'
            onInput={(detail) => setPassword(detail.value)}
          >
            {getLocale(S.KO_MACHINES_PASSWORD_LABEL)}
          </Input>
          <p className='help'>
            {hasSavedPassword
              ? getLocale(S.KO_MACHINES_PASSWORD_KEEP_HELP)
              : getLocale(S.KO_MACHINES_PASSWORD_HELP)}
          </p>
        </div>
        {hasSavedPassword && (
          <div>
            <Button
              kind='plain'
              size='small'
              onClick={forgetPassword}
            >
              {getLocale(S.KO_MACHINES_FORGET_PASSWORD)}
            </Button>
          </div>
        )}
        {formError && <Alert type='error'>{formError}</Alert>}
      </div>
      <div
        slot='actions'
        className='dialog-actions'
      >
        <Button
          kind='outline'
          onClick={props.onClose}
        >
          {getLocale(S.KO_MACHINES_CANCEL)}
        </Button>
        <Button
          isLoading={saving}
          onClick={save}
        >
          {getLocale(S.KO_MACHINES_SAVE)}
        </Button>
      </div>
    </Dialog>
  )
}
