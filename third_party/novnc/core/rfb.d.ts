// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

// Local declarations for the subset of the noVNC 1.7.0 RFB API (docs/API.md
// upstream) that K-O uses. rfb.js itself is unmodified.

export interface RFBCredentials {
  username?: string
  password?: string
  target?: string
}

export interface RFBOptions {
  shared?: boolean
  credentials?: RFBCredentials
  repeaterID?: string
  wsProtocols?: string[]
}

// Anything with the WebSocket / RTCDataChannel surface Websock.attach()
// checks: send, close, binaryType, onerror, onmessage, onopen, protocol and
// readyState.
export interface RFBRawChannel {
  binaryType: string
  readonly protocol: string
  readonly readyState: string | number
  onopen: ((event: Event) => void) | null
  onclose: ((event: CloseEvent) => void) | null
  onerror: ((event: Event) => void) | null
  onmessage: ((event: MessageEvent) => void) | null
  send(data: ArrayBuffer | ArrayBufferView): void
  close(): void
}

export interface RFBEventMap {
  connect: CustomEvent<Record<string, never>>
  disconnect: CustomEvent<{ clean: boolean }>
  credentialsrequired: CustomEvent<{ types: string[] }>
  securityfailure: CustomEvent<{ status: number; reason?: string }>
  serververification: CustomEvent<{ type: string }>
  clipboard: CustomEvent<{ text: string }>
  desktopname: CustomEvent<{ name: string }>
  bell: CustomEvent<Record<string, never>>
}

export default class RFB {
  constructor(
    target: HTMLElement,
    urlOrChannel: string | RFBRawChannel,
    options?: RFBOptions,
  )

  background: string
  clipViewport: boolean
  compressionLevel: number
  dragViewport: boolean
  focusOnClick: boolean
  qualityLevel: number
  resizeSession: boolean
  scaleViewport: boolean
  showDotCursor: boolean
  viewOnly: boolean

  addEventListener<K extends keyof RFBEventMap>(
    type: K,
    listener: (event: RFBEventMap[K]) => void,
  ): void
  removeEventListener<K extends keyof RFBEventMap>(
    type: K,
    listener: (event: RFBEventMap[K]) => void,
  ): void

  blur(): void
  clipboardPasteFrom(text: string): void
  disconnect(): void
  focus(options?: FocusOptions): void
  sendCredentials(credentials: RFBCredentials): void
}
