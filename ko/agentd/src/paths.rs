// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

//! K-O data dir layout and owner-only file helpers.

use std::fs::{self, DirBuilder, OpenOptions};
use std::io::{ErrorKind, Write};
use std::os::fd::AsRawFd;
use std::os::unix::fs::{DirBuilderExt, MetadataExt, OpenOptionsExt, PermissionsExt};
use std::path::{Path, PathBuf};

use anyhow::{Context, Result, bail};
use zeroize::Zeroizing;

/// Overrides the data dir (tests and development only).
pub const ENV_DATA_DIR: &str = "KO_AGENTD_DATA_DIR";

/// macOS: ~/Library/Application Support/K-O; Linux: $XDG_DATA_HOME/k-o
/// (fallback ~/.local/share/k-o).
pub fn default_data_dir() -> Result<PathBuf> {
    let base = dirs::data_dir().context("cannot determine the user data directory")?;
    #[cfg(target_os = "macos")]
    let name = "K-O";
    #[cfg(not(target_os = "macos"))]
    let name = "k-o";
    Ok(base.join(name))
}

#[derive(Clone, Debug)]
pub struct Paths {
    root: PathBuf,
}

impl Paths {
    pub fn new(root: impl Into<PathBuf>) -> Self {
        Self { root: root.into() }
    }

    /// CLI flag, then `KO_AGENTD_DATA_DIR`, then the platform default.
    pub fn resolve(cli: Option<PathBuf>) -> Result<Self> {
        if let Some(p) = cli {
            return Ok(Self::new(p));
        }
        if let Some(p) = std::env::var_os(ENV_DATA_DIR).filter(|v| !v.is_empty()) {
            return Ok(Self::new(PathBuf::from(p)));
        }
        Ok(Self::new(default_data_dir()?))
    }

    pub fn root(&self) -> &Path {
        &self.root
    }
    pub fn config(&self) -> PathBuf {
        self.root.join("agentd.toml")
    }
    pub fn token(&self) -> PathBuf {
        self.root.join("agentd.token")
    }
    pub fn socket(&self) -> PathBuf {
        self.root.join("agentd.sock")
    }
    pub fn port_file(&self) -> PathBuf {
        self.root.join("agentd.port")
    }
    pub fn db(&self) -> PathBuf {
        self.root.join("agentd.db")
    }
    pub fn secrets_file(&self) -> PathBuf {
        self.root.join("agentd.secrets.json")
    }
    pub fn artifacts_dir(&self) -> PathBuf {
        self.root.join("artifacts")
    }
    pub fn runs_dir(&self) -> PathBuf {
        self.root.join("runs")
    }
    pub fn tmp_dir(&self) -> PathBuf {
        self.root.join("tmp")
    }
    pub fn workspaces_dir(&self) -> PathBuf {
        self.root.join("workspaces")
    }
    pub fn lock_file(&self) -> PathBuf {
        self.root.join("agentd.lock")
    }

    /// Creates the data dir and its subdirs with 0700.
    pub fn ensure(&self) -> Result<()> {
        ensure_private_dir(&self.root)?;
        for d in [
            self.artifacts_dir(),
            self.runs_dir(),
            self.tmp_dir(),
            self.workspaces_dir(),
        ] {
            ensure_private_dir(&d)?;
        }
        Ok(())
    }

    /// Removes per-run temp dirs left behind by a crash.
    pub fn clean_tmp(&self) -> Result<()> {
        let dir = self.tmp_dir();
        if let Ok(entries) = fs::read_dir(&dir) {
            for e in entries.flatten() {
                let p = e.path();
                let meta = fs::symlink_metadata(&p)?;
                if meta.is_dir() {
                    fs::remove_dir_all(&p)
                        .with_context(|| format!("removing stale {}", p.display()))?;
                } else {
                    fs::remove_file(&p)?;
                }
            }
        }
        Ok(())
    }
}

/// Exclusive lock on `<data>/agentd.lock`, held while a daemon serves.
pub struct DataDirLock {
    _file: fs::File,
}

/// Fails if another daemon holds the data dir (its runs and temp dirs would
/// otherwise be marked failed and deleted).
pub fn lock_data_dir(paths: &Paths) -> Result<DataDirLock> {
    let p = paths.lock_file();
    let file = OpenOptions::new()
        .read(true)
        .write(true)
        .create(true)
        .truncate(false)
        .mode(0o600)
        .custom_flags(libc::O_NOFOLLOW)
        .open(&p)
        .with_context(|| format!("opening {}", p.display()))?;
    // SAFETY: flock on a valid fd owned by `file`.
    if unsafe { libc::flock(file.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) } != 0 {
        let e = std::io::Error::last_os_error();
        if e.kind() == ErrorKind::WouldBlock {
            bail!(
                "another ko-agentd is already running on {}",
                paths.root().display()
            );
        }
        return Err(e).with_context(|| format!("locking {}", p.display()));
    }
    Ok(DataDirLock { _file: file })
}

/// A 0700 dir under `<data>/tmp`, removed on drop.
pub struct PrivateTempDir {
    path: PathBuf,
}

impl PrivateTempDir {
    pub fn new(paths: &Paths, prefix: &str) -> Result<Self> {
        let path = paths
            .tmp_dir()
            .join(format!("{prefix}-{}", crate::util::random_hex(8)?));
        DirBuilder::new()
            .mode(0o700)
            .create(&path)
            .with_context(|| format!("creating {}", path.display()))?;
        Ok(Self { path })
    }

    pub fn path(&self) -> &Path {
        &self.path
    }
}

impl Drop for PrivateTempDir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.path);
    }
}

pub fn euid() -> u32 {
    // SAFETY: geteuid has no preconditions and cannot fail.
    unsafe { libc::geteuid() }
}

/// Creates `path` (0700) or verifies an existing dir is ours; tightens loose
/// permissions on an existing dir we own.
pub fn ensure_private_dir(path: &Path) -> Result<()> {
    match fs::metadata(path) {
        Ok(meta) => {
            if !meta.is_dir() {
                bail!("{} exists and is not a directory", path.display());
            }
            if meta.uid() != euid() {
                bail!("{} is not owned by the current user", path.display());
            }
            if meta.mode() & 0o077 != 0 {
                tracing::warn!(path = %path.display(), "tightening directory permissions to 0700");
                fs::set_permissions(path, fs::Permissions::from_mode(0o700))?;
            }
            Ok(())
        }
        Err(e) if e.kind() == ErrorKind::NotFound => {
            DirBuilder::new()
                .recursive(true)
                .mode(0o700)
                .create(path)
                .with_context(|| format!("creating {}", path.display()))?;
            fs::set_permissions(path, fs::Permissions::from_mode(0o700))?;
            Ok(())
        }
        Err(e) => Err(e).with_context(|| format!("stat {}", path.display())),
    }
}

/// Fails unless `path` is a regular file owned by us with no group/other bits.
pub fn check_private_file(path: &Path) -> Result<()> {
    let meta = fs::symlink_metadata(path).with_context(|| format!("stat {}", path.display()))?;
    if !meta.file_type().is_file() {
        bail!("{} is not a regular file", path.display());
    }
    if meta.uid() != euid() {
        bail!("{} is not owned by the current user", path.display());
    }
    if meta.mode() & 0o077 != 0 {
        bail!(
            "{} has permissions {:o}; it must be 0600",
            path.display(),
            meta.mode() & 0o777
        );
    }
    Ok(())
}

/// Reads an owner-only file. Fails closed on loose permissions.
pub fn read_private_file(path: &Path) -> Result<Zeroizing<Vec<u8>>> {
    check_private_file(path)?;
    Ok(Zeroizing::new(
        fs::read(path).with_context(|| format!("reading {}", path.display()))?,
    ))
}

/// Creates a new 0600 file; returns false if it already exists.
pub fn create_private_file_exclusive(path: &Path, data: &[u8]) -> Result<bool> {
    let mut f = match OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(path)
    {
        Ok(f) => f,
        Err(e) if e.kind() == ErrorKind::AlreadyExists => return Ok(false),
        Err(e) => return Err(e).with_context(|| format!("creating {}", path.display())),
    };
    f.write_all(data)?;
    f.sync_all()?;
    Ok(true)
}

/// Atomically replaces `path` with a 0600 file holding `data`.
pub fn write_private_file_atomic(path: &Path, data: &[u8]) -> Result<()> {
    let dir = path.parent().context("path has no parent")?;
    let name = path
        .file_name()
        .context("path has no file name")?
        .to_string_lossy();
    let tmp = dir.join(format!(".{name}.{}.tmp", crate::util::random_hex(8)?));
    let res = (|| -> Result<()> {
        let mut f = OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .open(&tmp)?;
        f.write_all(data)?;
        f.sync_all()?;
        fs::rename(&tmp, path)?;
        Ok(())
    })();
    if res.is_err() {
        let _ = fs::remove_file(&tmp);
    }
    res.with_context(|| format!("writing {}", path.display()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn private_dir_and_files() {
        let t = tempfile::tempdir().unwrap();
        let root = t.path().join("k-o");
        let paths = Paths::new(&root);
        paths.ensure().unwrap();
        let mode = fs::metadata(&root).unwrap().mode() & 0o777;
        assert_eq!(mode, 0o700);

        let f = root.join("x");
        assert!(create_private_file_exclusive(&f, b"a").unwrap());
        assert!(!create_private_file_exclusive(&f, b"b").unwrap());
        assert_eq!(&*read_private_file(&f).unwrap(), b"a");

        fs::set_permissions(&f, fs::Permissions::from_mode(0o644)).unwrap();
        assert!(read_private_file(&f).is_err());

        write_private_file_atomic(&f, b"c").unwrap();
        assert_eq!(&*read_private_file(&f).unwrap(), b"c");
    }

    #[test]
    fn private_temp_dir_is_removed() {
        let t = tempfile::tempdir().unwrap();
        let paths = Paths::new(t.path());
        paths.ensure().unwrap();
        let p = {
            let d = PrivateTempDir::new(&paths, "run").unwrap();
            assert_eq!(fs::metadata(d.path()).unwrap().mode() & 0o777, 0o700);
            fs::write(d.path().join("f"), b"x").unwrap();
            d.path().to_path_buf()
        };
        assert!(!p.exists());
    }

    #[test]
    fn loose_dir_is_tightened() {
        let t = tempfile::tempdir().unwrap();
        let d = t.path().join("d");
        fs::create_dir(&d).unwrap();
        fs::set_permissions(&d, fs::Permissions::from_mode(0o755)).unwrap();
        ensure_private_dir(&d).unwrap();
        assert_eq!(fs::metadata(&d).unwrap().mode() & 0o777, 0o700);
    }
}
