// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

//! Secret storage: macOS Keychain, or a 0600 file (tests/development only).

use std::collections::BTreeMap;
use std::path::PathBuf;
use std::sync::{Arc, Mutex};

use anyhow::{Context, Result, bail};
use zeroize::Zeroizing;

use crate::config::SecretBackendKind;
use crate::paths::{Paths, read_private_file, write_private_file_atomic};
use crate::util::valid_secret_name;

/// Keychain service name for all ko-agentd items.
pub const KEYCHAIN_SERVICE: &str = "K-O ko-agentd";

pub trait SecretBackend: Send + Sync {
    fn get(&self, name: &str) -> Result<Option<Zeroizing<String>>>;
    fn set(&self, name: &str, value: &str) -> Result<()>;
    fn delete(&self, name: &str) -> Result<bool>;
    fn kind(&self) -> &'static str;
}

#[derive(Clone)]
pub struct SecretStore {
    backend: Arc<dyn SecretBackend>,
}

impl SecretStore {
    pub fn open(kind: SecretBackendKind, paths: &Paths) -> Result<Self> {
        let backend: Arc<dyn SecretBackend> = match kind {
            SecretBackendKind::File => Arc::new(FileBackend::new(paths.secrets_file())),
            SecretBackendKind::Keychain => keychain_backend()?,
        };
        Ok(Self { backend })
    }

    pub fn kind(&self) -> &'static str {
        self.backend.kind()
    }

    /// Fetches a secret and registers it with the log/telemetry scrubber.
    pub fn get(&self, name: &str) -> Result<Option<Zeroizing<String>>> {
        check_name(name)?;
        let v = self.backend.get(name)?;
        if let Some(v) = &v {
            crate::telemetry::register_secret(v);
        }
        Ok(v)
    }

    pub fn require(&self, name: &str) -> Result<Zeroizing<String>> {
        self.get(name)?.with_context(|| {
            format!("secret {name:?} is not set (run: ko-agentd secrets set {name})")
        })
    }

    pub fn set(&self, name: &str, value: &str) -> Result<()> {
        check_name(name)?;
        if value.is_empty() {
            bail!("refusing to store an empty secret");
        }
        self.backend.set(name, value)
    }

    pub fn delete(&self, name: &str) -> Result<bool> {
        check_name(name)?;
        self.backend.delete(name)
    }

    pub async fn get_async(&self, name: &str) -> Result<Option<Zeroizing<String>>> {
        let this = self.clone();
        let name = name.to_owned();
        tokio::task::spawn_blocking(move || this.get(&name)).await?
    }

    pub async fn require_async(&self, name: &str) -> Result<Zeroizing<String>> {
        let this = self.clone();
        let name = name.to_owned();
        tokio::task::spawn_blocking(move || this.require(&name)).await?
    }
}

fn check_name(name: &str) -> Result<()> {
    if !valid_secret_name(name) {
        bail!("invalid secret name {name:?}");
    }
    Ok(())
}

/// JSON map in a 0600 file. Not encrypted: development/testing only.
pub struct FileBackend {
    path: PathBuf,
    lock: Mutex<()>,
}

impl FileBackend {
    pub fn new(path: PathBuf) -> Self {
        Self {
            path,
            lock: Mutex::new(()),
        }
    }

    fn load(&self) -> Result<BTreeMap<String, String>> {
        if !self.path.exists() {
            return Ok(BTreeMap::new());
        }
        let bytes = read_private_file(&self.path)?;
        serde_json::from_slice(&bytes).context("parsing secrets file")
    }

    fn store(&self, map: &BTreeMap<String, String>) -> Result<()> {
        let json = Zeroizing::new(serde_json::to_vec(map)?);
        write_private_file_atomic(&self.path, &json)
    }
}

impl SecretBackend for FileBackend {
    fn get(&self, name: &str) -> Result<Option<Zeroizing<String>>> {
        let _g = self.lock.lock().unwrap_or_else(|e| e.into_inner());
        let mut map = self.load()?;
        Ok(map.remove(name).map(Zeroizing::new))
    }

    fn set(&self, name: &str, value: &str) -> Result<()> {
        let _g = self.lock.lock().unwrap_or_else(|e| e.into_inner());
        let mut map = self.load()?;
        map.insert(name.to_owned(), value.to_owned());
        self.store(&map)
    }

    fn delete(&self, name: &str) -> Result<bool> {
        let _g = self.lock.lock().unwrap_or_else(|e| e.into_inner());
        let mut map = self.load()?;
        let had = map.remove(name).is_some();
        if had {
            self.store(&map)?;
        }
        Ok(had)
    }

    fn kind(&self) -> &'static str {
        "file"
    }
}

#[cfg(target_os = "macos")]
fn keychain_backend() -> Result<Arc<dyn SecretBackend>> {
    Ok(Arc::new(keychain::KeychainBackend))
}

#[cfg(not(target_os = "macos"))]
fn keychain_backend() -> Result<Arc<dyn SecretBackend>> {
    bail!("the Keychain backend is only available on macOS")
}

#[cfg(target_os = "macos")]
mod keychain {
    use super::*;

    pub struct KeychainBackend;

    fn entry(name: &str) -> Result<keyring::Entry> {
        keyring::Entry::new(KEYCHAIN_SERVICE, name).context("opening Keychain entry")
    }

    impl SecretBackend for KeychainBackend {
        fn get(&self, name: &str) -> Result<Option<Zeroizing<String>>> {
            match entry(name)?.get_password() {
                Ok(v) => Ok(Some(Zeroizing::new(v))),
                Err(keyring::Error::NoEntry) => Ok(None),
                Err(e) => Err(anyhow::anyhow!("Keychain read failed: {e}")),
            }
        }

        fn set(&self, name: &str, value: &str) -> Result<()> {
            entry(name)?
                .set_password(value)
                .map_err(|e| anyhow::anyhow!("Keychain write failed: {e}"))
        }

        fn delete(&self, name: &str) -> Result<bool> {
            match entry(name)?.delete_credential() {
                Ok(()) => Ok(true),
                Err(keyring::Error::NoEntry) => Ok(false),
                Err(e) => Err(anyhow::anyhow!("Keychain delete failed: {e}")),
            }
        }

        fn kind(&self) -> &'static str {
            "keychain"
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::MetadataExt;

    #[test]
    fn file_backend_roundtrip() {
        let t = tempfile::tempdir().unwrap();
        let paths = Paths::new(t.path());
        let s = SecretStore::open(SecretBackendKind::File, &paths).unwrap();
        assert!(s.get("nanogpt-api-key").unwrap().is_none());
        s.set("nanogpt-api-key", "sk-test-value-123").unwrap();
        assert_eq!(&*s.require("nanogpt-api-key").unwrap(), "sk-test-value-123");
        let mode = std::fs::metadata(paths.secrets_file()).unwrap().mode() & 0o777;
        assert_eq!(mode, 0o600);
        assert!(s.delete("nanogpt-api-key").unwrap());
        assert!(!s.delete("nanogpt-api-key").unwrap());
        assert!(s.set("Bad Name", "x").is_err());
        assert!(s.set("ok", "").is_err());
    }
}
