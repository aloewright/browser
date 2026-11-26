// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

import * as React from 'react'
import Alert from '@brave/leo/react/alert'
import Button from '@brave/leo/react/button'
import { getLocale } from '$web-common/locale'
import { mojom, pageHandler } from '../api'

// The user's own web apps (e.g. behind Cloudflare Access). The browser only
// navigates to them; Access handles sign-in.
export function WorkspaceApps() {
  const [apps, setApps] = React.useState<mojom.WorkspaceApp[]>([])
  const [reloadFailed, setReloadFailed] = React.useState(false)

  React.useEffect(() => {
    pageHandler.getWorkspaceApps().then(({ apps }) => setApps(apps))
  }, [])

  const reload = async () => {
    const { success, apps } = await pageHandler.reloadWorkspaceAppsFromFile()
    setReloadFailed(!success)
    setApps(apps)
  }

  return (
    <section className='card'>
      <div className='section-header'>
        <h2>{getLocale(S.KO_WORKSPACE_APPS)}</h2>
        <Button
          kind='plain-faint'
          size='small'
          onClick={reload}
        >
          {getLocale(S.KO_WORKSPACE_APPS_RELOAD)}
        </Button>
      </div>
      {reloadFailed && (
        <Alert type='warning'>{getLocale(S.KO_WORKSPACE_APPS_RELOAD_FAILED)}</Alert>
      )}
      {apps.length === 0 ? (
        <p className='muted'>{getLocale(S.KO_WORKSPACE_APPS_EMPTY)}</p>
      ) : (
        <ul className='list'>
          {apps.map((app) => (
            <li
              key={app.url.url}
              className='list-row'
            >
              <div className='list-text'>
                <span>{app.name}</span>
                <span className='muted small'>{new URL(app.url.url).host}</span>
              </div>
              <div className='row'>
                <Button
                  kind='outline'
                  size='small'
                  onClick={() => pageHandler.openWorkspaceApp(app.url, true)}
                >
                  {getLocale(S.KO_WORKSPACE_OPEN_IN_PANEL)}
                </Button>
                <Button
                  kind='outline'
                  size='small'
                  onClick={() => pageHandler.openWorkspaceApp(app.url, false)}
                >
                  {getLocale(S.KO_WORKSPACE_OPEN_IN_TAB)}
                </Button>
              </div>
            </li>
          ))}
        </ul>
      )}
    </section>
  )
}
