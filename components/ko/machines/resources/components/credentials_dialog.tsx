// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

import * as React from 'react'
import Alert from '@brave/leo/react/alert'
import Button from '@brave/leo/react/button'
import Dialog from '@brave/leo/react/dialog'
import Input from '@brave/leo/react/input'
import Toggle from '@brave/leo/react/toggle'
import { formatLocale, getLocale } from '$web-common/locale'
import '../strings'

export interface EnteredCredentials {
  username?: string
  password: string
  remember: boolean
}

interface Props {
  machineName: string
  // From noVNC's credentialsrequired event, e.g. ["username", "password"].
  types: string[]
  error?: string
  onSubmit: (credentials: EnteredCredentials) => void
  onCancel: () => void
}

export function CredentialsDialog(props: Props) {
  const needsUsername = props.types.includes('username')
  const [username, setUsername] = React.useState('')
  const [password, setPassword] = React.useState('')
  // Saving is a choice; it is never on by default.
  const [remember, setRemember] = React.useState(false)

  const submit = () => {
    if (!password || (needsUsername && !username)) {
      return
    }
    props.onSubmit({
      username: needsUsername ? username : undefined,
      password,
      remember,
    })
    setPassword('')
  }

  return (
    <Dialog
      isOpen
      backdropClickCloses={false}
      onClose={props.onCancel}
    >
      <div slot='title'>
        {formatLocale(S.KO_MACHINES_CREDENTIALS_TITLE, {
          $1: props.machineName,
        })}
      </div>
      <div
        className='form'
        onKeyDown={(e) => {
          if (
            e.key === 'Enter'
            && (e.target as HTMLElement).matches('leo-input')
          ) {
            submit()
          }
        }}
      >
        <p>
          {needsUsername
            ? getLocale(S.KO_MACHINES_CREDENTIALS_BODY_ARD)
            : getLocale(S.KO_MACHINES_CREDENTIALS_BODY_VNC)}
        </p>
        {props.error && <Alert type='error'>{props.error}</Alert>}
        {needsUsername && (
          <Input
            value={username}
            autocomplete='off'
            spellcheck={false}
            onInput={(detail) => setUsername(detail.value)}
          >
            {getLocale(S.KO_MACHINES_USERNAME_LABEL)}
          </Input>
        )}
        <Input
          type='password'
          value={password}
          autocomplete='off'
          onInput={(detail) => setPassword(detail.value)}
        >
          {getLocale(S.KO_MACHINES_PASSWORD_LABEL)}
        </Input>
        <Toggle
          checked={remember}
          onChange={({ checked }) => setRemember(checked)}
        >
          {getLocale(S.KO_MACHINES_REMEMBER)}
        </Toggle>
      </div>
      <div
        slot='actions'
        className='dialog-actions'
      >
        <Button
          kind='outline'
          onClick={props.onCancel}
        >
          {getLocale(S.KO_MACHINES_CANCEL)}
        </Button>
        <Button onClick={submit}>{getLocale(S.KO_MACHINES_SIGN_IN)}</Button>
      </div>
    </Dialog>
  )
}
