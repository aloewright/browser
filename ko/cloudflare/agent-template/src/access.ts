/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

// Cloudflare Access JWT validation (Cf-Access-Jwt-Assertion header).

import { createRemoteJWKSet, jwtVerify, type JWTVerifyGetKey } from 'jose'

export interface AccessConfig {
  teamDomain: string
  aud: string
}

export interface AccessIdentity {
  sub: string
  email?: string
  /** Service tokens carry a common_name instead of an email. */
  commonName?: string
}

const HOSTNAME =
  /^(?=.{1,253}$)([a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?\.)+[a-z]{2,63}$/

/** Returns a validated config, or null when the vars are missing/invalid. */
export function accessConfig(
  teamDomain: unknown,
  aud: unknown,
): AccessConfig | null {
  if (typeof teamDomain !== 'string' || typeof aud !== 'string') return null
  const domain = teamDomain.trim().toLowerCase()
  const tag = aud.trim()
  if (!HOSTNAME.test(domain) || !/^[A-Za-z0-9]{8,128}$/.test(tag)) return null
  return { teamDomain: domain, aud: tag }
}

const jwksCache = new Map<string, JWTVerifyGetKey>()

function remoteJwks(teamDomain: string): JWTVerifyGetKey {
  let jwks = jwksCache.get(teamDomain)
  if (!jwks) {
    jwks = createRemoteJWKSet(
      new URL(`https://${teamDomain}/cdn-cgi/access/certs`),
      {
        cooldownDuration: 30_000,
        cacheMaxAge: 10 * 60_000,
      },
    )
    jwksCache.set(teamDomain, jwks)
  }
  return jwks
}

/**
 * Verifies the Access JWT: RS256 signature from the team's certs, issuer,
 * audience (the application AUD tag) and expiry. Returns null on any failure.
 */
export async function verifyAccess(
  request: Request,
  cfg: AccessConfig,
  keys: JWTVerifyGetKey = remoteJwks(cfg.teamDomain),
): Promise<AccessIdentity | null> {
  const token = request.headers.get('cf-access-jwt-assertion')
  if (!token || token.length > 16_384) return null
  try {
    const { payload } = await jwtVerify(token, keys, {
      issuer: `https://${cfg.teamDomain}`,
      audience: cfg.aud,
      algorithms: ['RS256'],
      requiredClaims: ['exp', 'iat', 'sub'],
      clockTolerance: 5,
    })
    if (typeof payload.sub !== 'string') return null
    const identity: AccessIdentity = { sub: payload.sub }
    if (typeof payload.email === 'string') identity.email = payload.email
    if (typeof payload.common_name === 'string')
      identity.commonName = payload.common_name
    return identity
  } catch {
    return null
  }
}
