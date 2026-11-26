/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

import { beforeAll, describe, expect, it } from 'vitest'
import {
  SignJWT,
  createLocalJWKSet,
  exportJWK,
  generateKeyPair,
  type JWTVerifyGetKey,
} from 'jose'
import { accessConfig, verifyAccess } from '../src/access'
import { IDENTITY_HEADER, gate } from '../src/gate'

const TEAM = 'example.cloudflareaccess.com'
const AUD = 'aud0123456789abcdef'
const cfg = { teamDomain: TEAM, aud: AUD }

let keys: JWTVerifyGetKey
let priv: CryptoKey
let otherPriv: CryptoKey

beforeAll(async () => {
  const kp = await generateKeyPair('RS256', { extractable: true })
  const other = await generateKeyPair('RS256', { extractable: true })
  priv = kp.privateKey
  otherPriv = other.privateKey
  const jwk = { ...(await exportJWK(kp.publicKey)), kid: 'k1', alg: 'RS256' }
  keys = createLocalJWKSet({ keys: [jwk] })
})

async function token(
  opts: {
    aud?: string
    iss?: string
    exp?: string | number
    key?: CryptoKey
    email?: string
  } = {},
) {
  return new SignJWT({ email: opts.email ?? 'user@example.com', type: 'app' })
    .setProtectedHeader({ alg: 'RS256', kid: 'k1' })
    .setIssuer(opts.iss ?? `https://${TEAM}`)
    .setAudience(opts.aud ?? AUD)
    .setSubject('user-123')
    .setIssuedAt()
    .setExpirationTime(opts.exp ?? '5m')
    .sign(opts.key ?? priv)
}

function req(jwt?: string, extra: Record<string, string> = {}) {
  const headers: Record<string, string> = { ...extra }
  if (jwt) headers['cf-access-jwt-assertion'] = jwt
  return new Request('https://agent.example.com/chat', { headers })
}

describe('accessConfig', () => {
  it('requires both vars, no defaults', () => {
    expect(accessConfig(undefined, AUD)).toBeNull()
    expect(accessConfig(TEAM, undefined)).toBeNull()
    expect(accessConfig('', '')).toBeNull()
    expect(accessConfig('https://evil.example/x', AUD)).toBeNull()
    expect(accessConfig(TEAM, 'bad aud!')).toBeNull()
    expect(accessConfig(' Example.CloudflareAccess.com ', AUD)).toEqual(cfg)
  })
})

describe('verifyAccess', () => {
  it('accepts a valid token', async () => {
    const id = await verifyAccess(req(await token()), cfg, keys)
    expect(id).toEqual({ sub: 'user-123', email: 'user@example.com' })
  })

  it('rejects missing, wrong audience, issuer, expiry and signer', async () => {
    expect(await verifyAccess(req(), cfg, keys)).toBeNull()
    expect(
      await verifyAccess(req(await token({ aud: 'otheraud12345' })), cfg, keys),
    ).toBeNull()
    expect(
      await verifyAccess(
        req(await token({ iss: 'https://evil.cloudflareaccess.com' })),
        cfg,
        keys,
      ),
    ).toBeNull()
    expect(
      await verifyAccess(
        req(await token({ exp: Math.floor(Date.now() / 1000) - 60 })),
        cfg,
        keys,
      ),
    ).toBeNull()
    expect(
      await verifyAccess(req(await token({ key: otherPriv })), cfg, keys),
    ).toBeNull()
  })

  it('rejects alg=none and HS256 tokens', async () => {
    const b64 = (o: object) =>
      Buffer.from(JSON.stringify(o)).toString('base64url')
    const now = Math.floor(Date.now() / 1000)
    const claims = {
      iss: `https://${TEAM}`,
      aud: AUD,
      sub: 'x',
      iat: now,
      exp: now + 300,
    }
    const none = `${b64({ alg: 'none' })}.${b64(claims)}.`
    expect(await verifyAccess(req(none), cfg, keys)).toBeNull()
    const hs = await new SignJWT(claims)
      .setProtectedHeader({ alg: 'HS256' })
      .sign(new TextEncoder().encode('0123456789abcdef0123456789abcdef'))
    expect(await verifyAccess(req(hs), cfg, keys)).toBeNull()
  })

  it('rejects a tampered payload', async () => {
    const [h, , s] = (await token()).split('.')
    const forged = Buffer.from(
      JSON.stringify({
        iss: `https://${TEAM}`,
        aud: AUD,
        sub: 'admin',
        exp: 9999999999,
        iat: 1,
      }),
    ).toString('base64url')
    expect(await verifyAccess(req(`${h}.${forged}.${s}`), cfg, keys)).toBeNull()
  })
})

describe('gate', () => {
  it('fails closed without Access vars', async () => {
    const r = await gate(req(await token()), {}, keys)
    expect(r.ok).toBe(false)
    if (!r.ok) expect(r.response.status).toBe(500)
  })

  it('returns 403 without a valid token', async () => {
    const r = await gate(
      req(),
      { ACCESS_TEAM_DOMAIN: TEAM, ACCESS_AUD: AUD },
      keys,
    )
    expect(r.ok).toBe(false)
    if (!r.ok) expect(r.response.status).toBe(403)
  })

  it('overwrites a spoofed identity header', async () => {
    const r = await gate(
      req(await token(), { [IDENTITY_HEADER]: 'attacker' }),
      { ACCESS_TEAM_DOMAIN: TEAM, ACCESS_AUD: AUD },
      keys,
    )
    expect(r.ok).toBe(true)
    if (r.ok)
      expect(r.request.headers.get(IDENTITY_HEADER)).toBe('user@example.com')
  })
})
