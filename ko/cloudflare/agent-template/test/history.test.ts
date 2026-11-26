/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

import { describe, expect, it } from 'vitest'
import type { ChatMessage } from '../src/gateway'
import { trimHistory } from '../src/history'

const user = (n: number): ChatMessage => ({ role: 'user', content: `u${n}` })
const text = (n: number): ChatMessage => ({ role: 'assistant', content: `a${n}` })
const call = (id: string): ChatMessage => ({
  role: 'assistant',
  content: null,
  tool_calls: [{ id, type: 'function', function: { name: 't', arguments: '{}' } }],
})
const result = (id: string): ChatMessage => ({
  role: 'tool',
  content: 'ok',
  tool_call_id: id,
})

// Every tool result must follow an assistant message that requested it.
function valid(msgs: ChatMessage[]): boolean {
  const open = new Set<string>()
  for (const m of msgs) {
    if (m.role === 'assistant') m.tool_calls?.forEach((c) => open.add(c.id))
    if (m.role === 'tool' && !open.has(m.tool_call_id ?? '')) return false
  }
  return true
}

describe('trimHistory', () => {
  it('leaves short histories alone', () => {
    const h = [user(1), text(1)]
    expect(trimHistory(h, 10)).toBe(h)
  })

  it('never keeps a tool result without its tool call', () => {
    // Turn 1: user, call, result, answer. Turn 2: user, answer.
    const h = [user(1), call('c1'), result('c1'), text(1), user(2), text(2)]
    for (let max = 1; max <= h.length; max++) {
      const t = trimHistory(h, max)
      expect(valid(t), `max=${max}`).toBe(true)
      expect(t[0]?.role, `max=${max}`).toBe('user')
    }
    // A plain suffix of 4 would start at the orphaned tool result.
    expect(trimHistory(h, 4)).toEqual([user(2), text(2)])
  })

  it('keeps leading system messages', () => {
    const sys: ChatMessage = { role: 'system', content: 'be brief' }
    const h = [sys, user(1), text(1), user(2), text(2), user(3), text(3)]
    expect(trimHistory(h, 3)).toEqual([sys, user(3), text(3)])
  })

  it('keeps the latest whole turn when one turn exceeds the cap', () => {
    const h = [user(1), text(1), user(2), call('a'), result('a'), call('b'), result('b'), text(2)]
    const t = trimHistory(h, 3)
    expect(t).toEqual(h.slice(2))
    expect(valid(t)).toBe(true)
  })
})
