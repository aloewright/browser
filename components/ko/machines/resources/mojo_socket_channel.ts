// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

// A WebSocket-like channel over the two data pipes KOMachineBridge returns,
// in the shape noVNC's Websock.attach() accepts (send, close, binaryType,
// onopen, onclose, onerror, onmessage, protocol, readyState). It carries raw
// RFB bytes; there is no WebSocket framing and no websockify.

// The subset of the Mojo JS data pipe handle API used here. See
// third_party/blink/renderer/core/mojo/mojo_handle.idl.
export interface DataPipeHandle {
  close(): void
  watch(
    signals: { readable?: boolean; writable?: boolean; peerClosed?: boolean },
    callback: (result: number) => void,
  ): { cancel(): number }
  readData(
    buffer: ArrayBuffer | ArrayBufferView,
    options?: { allOrNone?: boolean; peek?: boolean },
  ): { result: number; numBytes: number }
  writeData(
    buffer: ArrayBuffer | ArrayBufferView,
    options?: { allOrNone?: boolean },
  ): { result: number; numBytes: number }
}

// Mojo result codes (mojo.idl).
const RESULT_OK = 0
const RESULT_SHOULD_WAIT = 17

const READ_CHUNK_SIZE = 64 * 1024
// Bound on bytes queued for a slow peer before the connection is dropped.
const MAX_QUEUED_SEND_BYTES = 8 * 1024 * 1024

type ReadyState = 'connecting' | 'open' | 'closing' | 'closed'
type Watcher = { cancel(): number }

export class MojoSocketChannel {
  binaryType: string = 'arraybuffer'
  onopen: ((event: Event) => void) | null = null
  onclose: ((event: CloseEvent) => void) | null = null
  onerror: ((event: Event) => void) | null = null

  private state: ReadyState = 'open'
  private messageHandler: ((event: MessageEvent) => void) | null = null
  private received: ArrayBuffer[] = []
  private sendQueue: Uint8Array[] = []
  private queuedBytes = 0
  private readWatcher: Watcher | null = null
  private writeWatcher: Watcher | null = null

  constructor(
    private readonly receiveStream: DataPipeHandle,
    private readonly sendStream: DataPipeHandle,
    // Drops the MachineConnection, which closes the socket in the browser.
    private readonly closeConnection: () => void,
  ) {
    this.readWatcher = this.receiveStream.watch(
      { readable: true, peerClosed: true },
      (result) => this.onReadable(result),
    )
  }

  get protocol(): string {
    return ''
  }

  get readyState(): ReadyState {
    return this.state
  }

  get bufferedAmount(): number {
    return this.queuedBytes
  }

  // Data that arrives before noVNC attaches is held until it does.
  get onmessage(): ((event: MessageEvent) => void) | null {
    return this.messageHandler
  }

  set onmessage(handler: ((event: MessageEvent) => void) | null) {
    this.messageHandler = handler
    this.deliverReceived()
  }

  send(data: ArrayBuffer | ArrayBufferView) {
    if (this.state !== 'open') {
      return
    }
    const bytes =
      data instanceof ArrayBuffer
        ? new Uint8Array(data)
        : new Uint8Array(data.buffer, data.byteOffset, data.byteLength)
    if (this.sendQueue.length === 0) {
      const written = this.write(bytes)
      if (written < 0 || written === bytes.byteLength) {
        return
      }
      // noVNC reuses its send buffer, so keep a copy of the rest.
      this.enqueue(bytes.slice(written))
    } else {
      this.enqueue(bytes.slice())
    }
  }

  close() {
    this.shutDown(1000, true)
  }

  // Returns the number of bytes written, or -1 if the pipe is gone.
  private write(bytes: Uint8Array): number {
    if (bytes.byteLength === 0) {
      return 0
    }
    const { result, numBytes } = this.sendStream.writeData(bytes)
    if (result === RESULT_OK) {
      return numBytes
    }
    if (result === RESULT_SHOULD_WAIT) {
      return 0
    }
    this.shutDown(1006, false)
    return -1
  }

  private enqueue(bytes: Uint8Array) {
    this.sendQueue.push(bytes)
    this.queuedBytes += bytes.byteLength
    if (this.queuedBytes > MAX_QUEUED_SEND_BYTES) {
      this.onerror?.(new Event('error'))
      this.shutDown(1006, false)
      return
    }
    if (!this.writeWatcher) {
      this.writeWatcher = this.sendStream.watch(
        { writable: true, peerClosed: true },
        (result) => this.onWritable(result),
      )
    }
  }

  private onWritable(result: number) {
    if (result !== RESULT_OK) {
      this.shutDown(1006, false)
      return
    }
    while (this.sendQueue.length > 0 && this.state === 'open') {
      const head = this.sendQueue[0]
      const written = this.write(head)
      if (written < 0) {
        return
      }
      this.queuedBytes -= written
      if (written < head.byteLength) {
        this.sendQueue[0] = head.subarray(written)
        return
      }
      this.sendQueue.shift()
    }
    this.writeWatcher?.cancel()
    this.writeWatcher = null
  }

  private onReadable(result: number) {
    if (result !== RESULT_OK) {
      this.shutDown(1006, false)
      return
    }
    while (this.state === 'open') {
      const buffer = new ArrayBuffer(READ_CHUNK_SIZE)
      const { result: readResult, numBytes } =
        this.receiveStream.readData(buffer)
      if (readResult === RESULT_SHOULD_WAIT) {
        return
      }
      if (readResult !== RESULT_OK) {
        // Peer closed and drained.
        this.shutDown(1006, false)
        return
      }
      if (numBytes > 0) {
        this.received.push(buffer.slice(0, numBytes))
        this.deliverReceived()
      }
    }
  }

  private deliverReceived() {
    while (this.messageHandler && this.received.length > 0) {
      const data = this.received.shift()!
      this.messageHandler(new MessageEvent('message', { data }))
    }
  }

  private shutDown(code: number, wasClean: boolean) {
    if (this.state === 'closing' || this.state === 'closed') {
      return
    }
    this.state = 'closing'
    if (!wasClean) {
      // The peer went away: hand over what already arrived first.
      this.deliverReceived()
    }
    this.readWatcher?.cancel()
    this.readWatcher = null
    this.writeWatcher?.cancel()
    this.writeWatcher = null
    this.receiveStream.close()
    this.sendStream.close()
    this.closeConnection()
    this.sendQueue = []
    this.queuedBytes = 0
    this.received = []
    this.state = 'closed'
    // WebSocket reports close asynchronously; noVNC expects the same.
    setTimeout(() => {
      this.onclose?.(new CloseEvent('close', { code, wasClean }))
    }, 0)
  }
}
