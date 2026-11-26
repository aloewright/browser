// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

use anyhow::{Result, anyhow};

pub fn random_bytes<const N: usize>() -> Result<[u8; N]> {
    let mut b = [0u8; N];
    getrandom::fill(&mut b).map_err(|e| anyhow!("OS RNG failure: {e}"))?;
    Ok(b)
}

pub fn random_hex(n: usize) -> Result<String> {
    let mut b = vec![0u8; n];
    getrandom::fill(&mut b).map_err(|e| anyhow!("OS RNG failure: {e}"))?;
    Ok(b.iter().map(|x| format!("{x:02x}")).collect())
}

pub fn now_ms() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as i64)
        .unwrap_or(0)
}

pub fn now_secs() -> i64 {
    now_ms() / 1000
}

pub fn rfc3339(ms: i64) -> String {
    let t = time::OffsetDateTime::from_unix_timestamp_nanos(ms as i128 * 1_000_000)
        .unwrap_or(time::OffsetDateTime::UNIX_EPOCH);
    t.format(&time::format_description::well_known::Rfc3339)
        .unwrap_or_default()
}

/// Truncates to at most `max` bytes on a char boundary.
pub fn truncate(s: &str, max: usize) -> &str {
    if s.len() <= max {
        return s;
    }
    let mut end = max;
    while !s.is_char_boundary(end) {
        end -= 1;
    }
    &s[..end]
}

/// Parses a UUID path id; rejects anything else (keeps ids safe for paths).
pub fn parse_id(s: &str) -> Option<String> {
    uuid::Uuid::parse_str(s)
        .ok()
        .filter(|u| u.hyphenated().to_string() == s)
        .map(|u| u.hyphenated().to_string())
}

pub fn new_id() -> String {
    uuid::Uuid::new_v4().hyphenated().to_string()
}

/// Secret names: lowercase, digits, '.', '_', '-'; 1..=64 chars.
pub fn valid_secret_name(s: &str) -> bool {
    !s.is_empty()
        && s.len() <= 64
        && s.bytes().next().is_some_and(|b| b.is_ascii_alphanumeric())
        && s.bytes().all(|b| {
            b.is_ascii_lowercase() || b.is_ascii_digit() || matches!(b, b'.' | b'_' | b'-')
        })
}

/// Identifier for MCP server names, env var names, etc.
pub fn valid_ident(s: &str, max: usize) -> bool {
    !s.is_empty()
        && s.len() <= max
        && s.bytes()
            .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'_' | b'-'))
}

/// Model names passed to CLIs and upstream APIs: never start with '-'.
/// '[' and ']' are for Claude Code aliases such as "sonnet[1m]".
pub fn valid_model_name(s: &str) -> bool {
    !s.is_empty()
        && s.len() <= 128
        && !s.starts_with('-')
        && s.bytes().all(|b| {
            b.is_ascii_alphanumeric()
                || matches!(b, b'.' | b'_' | b'-' | b':' | b'/' | b'@' | b'[' | b']')
        })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ids_and_names() {
        let id = new_id();
        assert_eq!(parse_id(&id).as_deref(), Some(id.as_str()));
        assert!(parse_id("../etc").is_none());
        assert!(parse_id(&id.to_uppercase()).is_none());
        assert!(valid_secret_name("nanogpt-api-key"));
        assert!(!valid_secret_name("Bad Name"));
        assert!(!valid_secret_name("-x"));
        assert!(valid_model_name("claude-sonnet-4-5"));
        // Claude Code's 1M-context aliases.
        assert!(valid_model_name("sonnet[1m]"));
        assert!(valid_model_name("opus[1m]"));
        assert!(!valid_model_name("--dangerously-skip-permissions"));
        assert_eq!(truncate("héllo", 2), "h");
    }
}
