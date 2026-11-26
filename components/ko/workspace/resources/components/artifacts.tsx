// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

import * as React from 'react'
import Alert from '@brave/leo/react/alert'
import Button from '@brave/leo/react/button'
import ProgressRing from '@brave/leo/react/progressRing'
import { getLocale } from '$web-common/locale'
import {
  Artifact,
  base64ToBytes,
  errorMessage,
  getAgentd,
  mojom,
  pageHandler,
} from '../api'

// Matches agentd's 32 MiB artifact limit after base64.
const kMaxUploadBytes = 20 * 1024 * 1024

interface Preview {
  name: string
  mimeType: string
  url?: string
  text?: string
}

function revoke(preview?: Preview) {
  if (preview?.url) {
    URL.revokeObjectURL(preview.url)
  }
}

export function Artifacts(props: { workspaceId: string }) {
  const [artifacts, setArtifacts] = React.useState<Artifact[]>()
  const [busy, setBusy] = React.useState(false)
  const [error, setError] = React.useState<string>()
  const [preview, setPreview] = React.useState<Preview>()
  const fileInput = React.useRef<HTMLInputElement>(null)

  const refresh = React.useCallback(async () => {
    try {
      setArtifacts(await getAgentd().listArtifacts(props.workspaceId))
    } catch (e) {
      setError(errorMessage(e))
      setArtifacts([])
    }
  }, [props.workspaceId])

  React.useEffect(() => {
    refresh()
  }, [refresh])

  React.useEffect(() => () => revoke(preview), [preview])

  const upload = async (name: string, mimeType: string, bytes: Uint8Array) => {
    setBusy(true)
    setError(undefined)
    try {
      await getAgentd().uploadArtifact(props.workspaceId, name, mimeType, bytes)
      await refresh()
    } catch (e) {
      setError(errorMessage(e))
    } finally {
      setBusy(false)
    }
  }

  const onFileChosen = async () => {
    const file = fileInput.current?.files?.[0]
    if (fileInput.current) {
      fileInput.current.value = ''
    }
    if (!file || file.size > kMaxUploadBytes) {
      return
    }
    await upload(
      file.name,
      file.type,
      new Uint8Array(await file.arrayBuffer()),
    )
  }

  const capture = async (kind: mojom.CaptureKind) => {
    setError(undefined)
    const { capture } = await pageHandler.captureActiveTab(kind)
    if (!capture) {
      setError(getLocale(S.KO_WORKSPACE_CAPTURE_UNAVAILABLE))
      return
    }
    await upload(
      capture.fileName,
      capture.mimeType,
      new Uint8Array(capture.data),
    )
  }

  const view = async (artifact: Artifact) => {
    try {
      const full = await getAgentd().getArtifact(props.workspaceId, artifact.id)
      const bytes = base64ToBytes(full.content_base64 ?? '')
      const mimeType = full.mime_type ?? 'application/octet-stream'
      // Never render stored HTML/MHTML: images and text only.
      if (mimeType.startsWith('image/') && mimeType !== 'image/svg+xml') {
        const url = URL.createObjectURL(new Blob([bytes], { type: mimeType }))
        setPreview({ name: full.name, mimeType, url })
      } else if (mimeType.startsWith('text/') || mimeType.endsWith('json')) {
        setPreview({
          name: full.name,
          mimeType,
          text: new TextDecoder().decode(bytes),
        })
      } else {
        const url = URL.createObjectURL(
          new Blob([bytes], { type: 'application/octet-stream' }),
        )
        const link = document.createElement('a')
        link.href = url
        link.download = full.name
        link.click()
        setTimeout(() => URL.revokeObjectURL(url), 0)
      }
    } catch (e) {
      setError(errorMessage(e))
    }
  }

  const remove = async (artifact: Artifact) => {
    try {
      await getAgentd().deleteArtifact(props.workspaceId, artifact.id)
      if (preview?.name === artifact.name) {
        setPreview(undefined)
      }
      await refresh()
    } catch (e) {
      setError(errorMessage(e))
    }
  }

  return (
    <section className='card'>
      <h2>{getLocale(S.KO_WORKSPACE_ARTIFACTS)}</h2>
      <div className='row wrap'>
        <Button
          kind='outline'
          size='small'
          isDisabled={busy}
          onClick={() => fileInput.current?.click()}
        >
          {getLocale(S.KO_WORKSPACE_UPLOAD_FILE)}
        </Button>
        <Button
          kind='outline'
          size='small'
          isDisabled={busy}
          onClick={() => capture(mojom.CaptureKind.kPage)}
        >
          {getLocale(S.KO_WORKSPACE_CAPTURE_PAGE)}
        </Button>
        <Button
          kind='outline'
          size='small'
          isDisabled={busy}
          onClick={() => capture(mojom.CaptureKind.kScreenshot)}
        >
          {getLocale(S.KO_WORKSPACE_CAPTURE_SCREENSHOT)}
        </Button>
        {busy && <ProgressRing />}
        <input
          ref={fileInput}
          type='file'
          hidden
          onChange={onFileChosen}
        />
      </div>
      {error && <Alert type='error'>{error}</Alert>}
      {artifacts === undefined ? (
        <ProgressRing />
      ) : artifacts.length === 0 ? (
        <p className='muted'>{getLocale(S.KO_WORKSPACE_ARTIFACTS_EMPTY)}</p>
      ) : (
        <ul className='list'>
          {artifacts.map((artifact) => (
            <li
              key={artifact.id}
              className='list-row'
            >
              <div className='list-text'>
                <span>{artifact.name}</span>
                <span className='muted small'>{artifact.mime_type}</span>
              </div>
              <div className='row'>
                <Button
                  kind='plain-faint'
                  size='small'
                  onClick={() => view(artifact)}
                >
                  {getLocale(S.KO_WORKSPACE_VIEW)}
                </Button>
                <Button
                  kind='plain-faint'
                  size='small'
                  onClick={() => remove(artifact)}
                >
                  {getLocale(S.KO_WORKSPACE_DELETE)}
                </Button>
              </div>
            </li>
          ))}
        </ul>
      )}
      {preview && (
        <figure className='preview'>
          <figcaption className='muted small'>{preview.name}</figcaption>
          {preview.url ? (
            <img
              src={preview.url}
              alt={preview.name}
            />
          ) : (
            <pre>{preview.text}</pre>
          )}
        </figure>
      )}
    </section>
  )
}
