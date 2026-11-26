/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

// Front door: every request must carry a valid Access JWT. Fails closed when
// the Access vars are not configured.

import { accessConfig, verifyAccess, type AccessIdentity } from './access'
import type { JWTVerifyGetKey } from 'jose'

export const IDENTITY_HEADER = 'x-ko-access-identity'

export interface GateEnv {
  ACCESS_TEAM_DOMAIN?: string
  ACCESS_AUD?: string
}

export function json(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: {
      'content-type': 'application/json',
      'cache-control': 'no-store',
      'x-content-type-options': 'nosniff',
    },
  })
}

export type GateResult =
  | { ok: true; request: Request; identity: AccessIdentity }
  | { ok: false; response: Response }

export async function gate(
  request: Request,
  env: GateEnv,
  keys?: JWTVerifyGetKey,
): Promise<GateResult> {
  const cfg = accessConfig(env.ACCESS_TEAM_DOMAIN, env.ACCESS_AUD)
  if (!cfg) {
    return {
      ok: false,
      response: json({ error: 'access is not configured' }, 500),
    }
  }
  const identity = await verifyAccess(request, cfg, keys)
  if (!identity) {
    return { ok: false, response: json({ error: 'forbidden' }, 403) }
  }
  // Replace any client-supplied identity header with the verified one.
  const headers = new Headers(request.headers)
  headers.set(
    IDENTITY_HEADER,
    identity.email ?? identity.commonName ?? identity.sub,
  )
  return { ok: true, request: new Request(request, { headers }), identity }
}
