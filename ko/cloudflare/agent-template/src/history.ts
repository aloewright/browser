/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

import type { ChatMessage } from './gateway'

/**
 * Caps stored history at about `max` messages without splitting a turn. The
 * kept part starts on a user message, so a tool result is never kept without
 * the assistant message whose tool_calls it answers. Leading system messages
 * are always kept. If the window holds no user message, the latest whole turn
 * is kept even past `max`; MAX_TOOL_ROUNDS bounds its size.
 */
export function trimHistory(
  messages: ChatMessage[],
  max: number,
): ChatMessage[] {
  if (messages.length <= max) return messages
  let lead = 0
  while (messages[lead]?.role === 'system') lead++
  const system = messages.slice(0, lead)
  const rest = messages.slice(lead)
  let start = Math.max(0, rest.length - Math.max(1, max - lead))
  while (start < rest.length && rest[start]?.role !== 'user') start++
  if (start === rest.length) {
    start = rest.length - 1
    while (start > 0 && rest[start]?.role !== 'user') start--
  }
  return [...system, ...rest.slice(start)]
}
