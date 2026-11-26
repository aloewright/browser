// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

import * as mojom from 'gen/brave/components/ko/machines/common/ko_machines.mojom.m.js'
import { DataPipeHandle, MojoSocketChannel } from './mojo_socket_channel'

export const MachineKind = {
  ScreenSharing: mojom.MachineKind.kScreenSharing as number,
  Vnc: mojom.MachineKind.kVnc as number,
}

export const MachineError = {
  None: mojom.MachineError.kNone as number,
  InvalidName: mojom.MachineError.kInvalidName as number,
  InvalidHost: mojom.MachineError.kInvalidHost as number,
  InvalidPort: mojom.MachineError.kInvalidPort as number,
  Duplicate: mojom.MachineError.kDuplicate as number,
  TooMany: mojom.MachineError.kTooMany as number,
  NotFound: mojom.MachineError.kNotFound as number,
  InvalidCredentials: mojom.MachineError.kInvalidCredentials as number,
  EncryptionUnavailable: mojom.MachineError.kEncryptionUnavailable as number,
}

export const ImportStatus = {
  Imported: mojom.ImportStatus.kImported as number,
  NotFound: mojom.ImportStatus.kNotFound as number,
  InsecureFile: mojom.ImportStatus.kInsecureFile as number,
  InvalidFile: mojom.ImportStatus.kInvalidFile as number,
}

export const ConnectResult = {
  Ok: mojom.ConnectResult.kOk as number,
  UnknownMachine: mojom.ConnectResult.kUnknownMachine as number,
  NoAllowlist: mojom.ConnectResult.kNoAllowlist as number,
  ResolveFailed: mojom.ConnectResult.kResolveFailed as number,
  NotAllowed: mojom.ConnectResult.kNotAllowed as number,
  ConnectFailed: mojom.ConnectResult.kConnectFailed as number,
  TooManyConnections: mojom.ConnectResult.kTooManyConnections as number,
}

export interface Machine {
  id: string
  name: string
  host: string
  port: number
  kind: number
  hasCredentials: boolean
}

export interface MachineInput {
  name: string
  host: string
  port: number
  kind: number
}

export interface MachinesState {
  machines: Machine[]
  allowedCidrs: string[]
  rejectedCidrs: string[]
}

// The shapes of the generated remotes (ko_machines.mojom).
interface MachinesHandlerRemote {
  setPage(page: unknown): void
  getMachines(): Promise<MachinesState>
  addMachine(
    input: MachineInput,
  ): Promise<{ error: number; id?: string | null }>
  updateMachine(id: string, input: MachineInput): Promise<{ error: number }>
  removeMachine(id: string): Promise<{ removed: boolean }>
  setCredentials(
    id: string,
    username: string | null,
    password: string,
  ): Promise<{ error: number }>
  clearCredentials(id: string): Promise<unknown>
  getCredentials(
    id: string,
  ): Promise<{ username?: string | null; password?: string | null }>
  importFromFile(): Promise<{
    status: number
    added: number
    allowlistUpdated: boolean
  }>
}

interface KOMachineBridgeRemote {
  connect(
    machineId: string,
    connection: unknown,
  ): Promise<{
    result: number
    receiveStream?: DataPipeHandle | null
    sendStream?: DataPipeHandle | null
  }>
}

interface MachineConnectionRemote {
  $: {
    bindNewPipeAndPassReceiver(): unknown
    close(): void
  }
}

interface MachinesPageCallbackRouter {
  $: { bindNewPipeAndPassRemote(): unknown }
  onMachinesChanged: { addListener(listener: () => void): number }
}

let handler: MachinesHandlerRemote | undefined
let bridge: KOMachineBridgeRemote | undefined

export function getHandler(): MachinesHandlerRemote {
  if (!handler) {
    handler =
      mojom.MachinesHandler.getRemote() as unknown as MachinesHandlerRemote
  }
  return handler
}

function getBridge(): KOMachineBridgeRemote {
  if (!bridge) {
    bridge =
      mojom.KOMachineBridge.getRemote() as unknown as KOMachineBridgeRemote
  }
  return bridge
}

// Calls `listener` whenever machines, credentials or the allowlist change.
export function listenForChanges(listener: () => void) {
  const router =
    new mojom.MachinesPageCallbackRouter() as unknown as MachinesPageCallbackRouter
  router.onMachinesChanged.addListener(listener)
  getHandler().setPage(router.$.bindNewPipeAndPassRemote())
}

export interface Connection {
  result: number
  channel?: MojoSocketChannel
}

// Asks the browser for a socket to the machine. The page only names the
// machine; the browser picks and vets the address.
export async function connectToMachine(machineId: string): Promise<Connection> {
  const connection =
    new mojom.MachineConnectionRemote() as unknown as MachineConnectionRemote
  const receiver = connection.$.bindNewPipeAndPassReceiver()
  const { result, receiveStream, sendStream } = await getBridge().connect(
    machineId,
    receiver,
  )
  if (result !== ConnectResult.Ok || !receiveStream || !sendStream) {
    receiveStream?.close()
    sendStream?.close()
    connection.$.close()
    return {
      result:
        result === ConnectResult.Ok ? ConnectResult.ConnectFailed : result,
    }
  }
  return {
    result,
    channel: new MojoSocketChannel(receiveStream, sendStream, () =>
      connection.$.close(),
    ),
  }
}

export function openDesktop(machineId: string) {
  window.open(`/view?id=${encodeURIComponent(machineId)}`, '_blank', 'noopener')
}
