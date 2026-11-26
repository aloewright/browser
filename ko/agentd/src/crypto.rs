// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

//! AES-256-GCM sealing for artifacts and run logs. Each blob has a random
//! 96-bit nonce and binds its identity through AAD.

use aes_gcm::aead::{Aead, KeyInit, Payload};
use aes_gcm::{Aes256Gcm, Nonce};
use anyhow::{Result, anyhow, bail};
use base64::Engine;
use base64::engine::general_purpose::STANDARD;
use zeroize::Zeroizing;

use crate::secrets::SecretStore;

/// Keychain item holding the storage key (base64).
pub const STORAGE_KEY_SECRET: &str = "storage-key";

const MAGIC: &[u8; 4] = b"KOE1";
const NONCE_LEN: usize = 12;
const HEADER_LEN: usize = MAGIC.len() + NONCE_LEN;

pub struct StorageKey {
    cipher: Aes256Gcm,
}

impl StorageKey {
    pub fn from_bytes(key: &[u8; 32]) -> Self {
        Self {
            cipher: Aes256Gcm::new(key.into()),
        }
    }

    pub fn generate_raw() -> Result<Zeroizing<[u8; 32]>> {
        Ok(Zeroizing::new(crate::util::random_bytes::<32>()?))
    }

    pub fn from_b64(s: &str) -> Result<Self> {
        let raw = Zeroizing::new(
            STANDARD
                .decode(s.trim())
                .map_err(|_| anyhow!("storage key is not valid base64"))?,
        );
        let arr: [u8; 32] = raw
            .as_slice()
            .try_into()
            .map_err(|_| anyhow!("storage key must be 32 bytes"))?;
        let arr = Zeroizing::new(arr);
        Ok(Self::from_bytes(&arr))
    }

    /// Output: "KOE1" | nonce | ciphertext+tag.
    pub fn seal(&self, aad: &[u8], plaintext: &[u8]) -> Result<Vec<u8>> {
        let nonce = crate::util::random_bytes::<NONCE_LEN>()?;
        let ct = self
            .cipher
            .encrypt(
                Nonce::from_slice(&nonce),
                Payload {
                    msg: plaintext,
                    aad,
                },
            )
            .map_err(|_| anyhow!("encryption failed"))?;
        let mut out = Vec::with_capacity(HEADER_LEN + ct.len());
        out.extend_from_slice(MAGIC);
        out.extend_from_slice(&nonce);
        out.extend_from_slice(&ct);
        Ok(out)
    }

    pub fn open(&self, aad: &[u8], blob: &[u8]) -> Result<Zeroizing<Vec<u8>>> {
        if blob.len() < HEADER_LEN + 16 || &blob[..4] != MAGIC {
            bail!("encrypted blob is malformed");
        }
        let nonce = Nonce::from_slice(&blob[4..HEADER_LEN]);
        let pt = self
            .cipher
            .decrypt(
                nonce,
                Payload {
                    msg: &blob[HEADER_LEN..],
                    aad,
                },
            )
            .map_err(|_| {
                anyhow!("decryption failed: data was tampered with or the key is wrong")
            })?;
        Ok(Zeroizing::new(pt))
    }
}

/// Loads the storage key from the secret store, creating it on first run.
/// `has_encrypted_data` guards against silently replacing a lost key.
pub fn load_or_create_storage_key(
    secrets: &SecretStore,
    has_encrypted_data: bool,
) -> Result<StorageKey> {
    if let Some(v) = secrets.get(STORAGE_KEY_SECRET)? {
        return StorageKey::from_b64(&v);
    }
    if has_encrypted_data {
        bail!(
            "the storage key is missing from the {} secret store but encrypted data exists; \
             refusing to create a new key",
            secrets.kind()
        );
    }
    let raw = StorageKey::generate_raw()?;
    let b64 = Zeroizing::new(STANDARD.encode(raw.as_slice()));
    secrets.set(STORAGE_KEY_SECRET, &b64)?;
    Ok(StorageKey::from_bytes(&raw))
}

pub fn artifact_aad(workspace_id: &str, artifact_id: &str) -> Vec<u8> {
    format!("ko-agentd/artifact/v1\0{workspace_id}\0{artifact_id}").into_bytes()
}

pub fn run_event_aad(run_id: &str, seq: u64) -> Vec<u8> {
    format!("ko-agentd/run-event/v1\0{run_id}\0{seq}").into_bytes()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn key() -> StorageKey {
        StorageKey::from_bytes(&[7u8; 32])
    }

    #[test]
    fn roundtrip() {
        let k = key();
        let aad = artifact_aad("w", "a");
        let blob = k.seal(&aad, b"hello world").unwrap();
        assert_ne!(&blob[HEADER_LEN..], b"hello world");
        assert_eq!(&*k.open(&aad, &blob).unwrap(), b"hello world");
        // Random nonces: sealing twice differs.
        assert_ne!(blob, k.seal(&aad, b"hello world").unwrap());
    }

    #[test]
    fn detects_tampering() {
        let k = key();
        let aad = artifact_aad("w", "a");
        let blob = k.seal(&aad, b"secret data").unwrap();
        for i in [4, HEADER_LEN, blob.len() - 1] {
            let mut t = blob.clone();
            t[i] ^= 1;
            assert!(k.open(&aad, &t).is_err(), "byte {i}");
        }
        // Swapped identity (AAD) is rejected.
        assert!(k.open(&artifact_aad("w", "b"), &blob).is_err());
        // Truncation and wrong key are rejected.
        assert!(k.open(&aad, &blob[..blob.len() - 1]).is_err());
        assert!(
            StorageKey::from_bytes(&[8u8; 32])
                .open(&aad, &blob)
                .is_err()
        );
        assert!(k.open(&aad, b"KOE1short").is_err());
    }

    #[test]
    fn key_from_store() {
        let t = tempfile::tempdir().unwrap();
        let paths = crate::paths::Paths::new(t.path());
        let s = SecretStore::open(crate::config::SecretBackendKind::File, &paths).unwrap();
        assert!(load_or_create_storage_key(&s, true).is_err());
        let k1 = load_or_create_storage_key(&s, false).unwrap();
        let blob = k1.seal(b"x", b"y").unwrap();
        let k2 = load_or_create_storage_key(&s, true).unwrap();
        assert_eq!(&*k2.open(b"x", &blob).unwrap(), b"y");
    }
}
