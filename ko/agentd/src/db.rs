// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

//! SQLite index: workspaces, artifact metadata, runs, API-added MCP servers.
//! Artifact contents and run event logs are stored encrypted on disk, not
//! here.

use std::path::Path;
use std::sync::{Arc, Mutex};

use anyhow::{Context, Result};
use rusqlite::{Connection, OptionalExtension, params};
use serde::{Serialize, Serializer};

use crate::util::rfc3339;

fn ser_ts<S: Serializer>(ms: &i64, s: S) -> Result<S::Ok, S::Error> {
    s.serialize_str(&rfc3339(*ms))
}

fn ser_opt_ts<S: Serializer>(ms: &Option<i64>, s: S) -> Result<S::Ok, S::Error> {
    match ms {
        Some(v) => s.serialize_some(&rfc3339(*v)),
        None => s.serialize_none(),
    }
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct CloudflareAgentInfo {
    pub script_name: String,
    pub url: String,
    #[serde(serialize_with = "ser_ts")]
    pub deployed_at: i64,
}

#[derive(Debug, Clone, Serialize)]
pub struct Workspace {
    pub id: String,
    pub name: String,
    /// Brave Containers id the browser opens this workspace's tabs in.
    pub container_id: Option<String>,
    pub tags: Vec<String>,
    pub default_model: Option<String>,
    pub mcp_servers: Vec<String>,
    pub cloudflare_agent: Option<CloudflareAgentInfo>,
    #[serde(serialize_with = "ser_ts")]
    pub created_at: i64,
    #[serde(serialize_with = "ser_ts")]
    pub updated_at: i64,
}

#[derive(Debug, Clone, Serialize)]
pub struct ArtifactMeta {
    pub id: String,
    pub workspace_id: String,
    pub name: String,
    pub mime_type: String,
    pub size: i64,
    pub tags: Vec<String>,
    #[serde(serialize_with = "ser_ts")]
    pub created_at: i64,
}

#[derive(Debug, Clone, Serialize)]
pub struct Run {
    pub id: String,
    pub workspace_id: String,
    pub model: String,
    pub mcp_servers: Vec<String>,
    pub status: String,
    pub error: Option<String>,
    pub usage: Option<serde_json::Value>,
    #[serde(serialize_with = "ser_ts")]
    pub created_at: i64,
    #[serde(serialize_with = "ser_opt_ts")]
    pub started_at: Option<i64>,
    #[serde(serialize_with = "ser_opt_ts")]
    pub finished_at: Option<i64>,
}

#[derive(Clone)]
pub struct Db {
    conn: Arc<Mutex<Connection>>,
}

const SCHEMA: &str = r#"
CREATE TABLE IF NOT EXISTS meta (
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS workspaces (
  id TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  container_id TEXT,
  tags TEXT NOT NULL DEFAULT '[]',
  default_model TEXT,
  mcp_servers TEXT NOT NULL DEFAULT '[]',
  cf_script TEXT,
  cf_url TEXT,
  cf_deployed_at INTEGER,
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS artifacts (
  id TEXT PRIMARY KEY,
  workspace_id TEXT NOT NULL REFERENCES workspaces(id) ON DELETE CASCADE,
  name TEXT NOT NULL,
  mime_type TEXT NOT NULL,
  size INTEGER NOT NULL,
  tags TEXT NOT NULL DEFAULT '[]',
  created_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS artifacts_ws ON artifacts(workspace_id, created_at);
CREATE TABLE IF NOT EXISTS runs (
  id TEXT PRIMARY KEY,
  workspace_id TEXT NOT NULL REFERENCES workspaces(id) ON DELETE CASCADE,
  model TEXT NOT NULL,
  mcp_servers TEXT NOT NULL DEFAULT '[]',
  status TEXT NOT NULL,
  error TEXT,
  usage TEXT,
  created_at INTEGER NOT NULL,
  started_at INTEGER,
  finished_at INTEGER
);
CREATE INDEX IF NOT EXISTS runs_ws ON runs(workspace_id, created_at);
CREATE TABLE IF NOT EXISTS mcp_servers (
  name TEXT PRIMARY KEY,
  definition TEXT NOT NULL,
  created_at INTEGER NOT NULL
);
"#;

fn json_vec(s: String) -> Vec<String> {
    serde_json::from_str(&s).unwrap_or_default()
}

fn like_pattern(q: &str) -> String {
    let mut out = String::with_capacity(q.len() + 2);
    out.push('%');
    for c in q.chars() {
        if matches!(c, '\\' | '%' | '_') {
            out.push('\\');
        }
        out.push(c);
    }
    out.push('%');
    out
}

fn row_workspace(r: &rusqlite::Row<'_>) -> rusqlite::Result<Workspace> {
    let cf_script: Option<String> = r.get("cf_script")?;
    let cf_url: Option<String> = r.get("cf_url")?;
    let cf_at: Option<i64> = r.get("cf_deployed_at")?;
    Ok(Workspace {
        id: r.get("id")?,
        name: r.get("name")?,
        container_id: r.get("container_id")?,
        tags: json_vec(r.get("tags")?),
        default_model: r.get("default_model")?,
        mcp_servers: json_vec(r.get("mcp_servers")?),
        cloudflare_agent: match (cf_script, cf_url, cf_at) {
            (Some(script_name), Some(url), Some(deployed_at)) => Some(CloudflareAgentInfo {
                script_name,
                url,
                deployed_at,
            }),
            _ => None,
        },
        created_at: r.get("created_at")?,
        updated_at: r.get("updated_at")?,
    })
}

fn row_artifact(r: &rusqlite::Row<'_>) -> rusqlite::Result<ArtifactMeta> {
    Ok(ArtifactMeta {
        id: r.get("id")?,
        workspace_id: r.get("workspace_id")?,
        name: r.get("name")?,
        mime_type: r.get("mime_type")?,
        size: r.get("size")?,
        tags: json_vec(r.get("tags")?),
        created_at: r.get("created_at")?,
    })
}

fn row_run(r: &rusqlite::Row<'_>) -> rusqlite::Result<Run> {
    let usage: Option<String> = r.get("usage")?;
    Ok(Run {
        id: r.get("id")?,
        workspace_id: r.get("workspace_id")?,
        model: r.get("model")?,
        mcp_servers: json_vec(r.get("mcp_servers")?),
        status: r.get("status")?,
        error: r.get("error")?,
        usage: usage.and_then(|u| serde_json::from_str(&u).ok()),
        created_at: r.get("created_at")?,
        started_at: r.get("started_at")?,
        finished_at: r.get("finished_at")?,
    })
}

impl Db {
    pub fn open(path: &Path) -> Result<Self> {
        // Create the file 0600 first; SQLite's WAL/SHM files inherit its mode.
        crate::paths::create_private_file_exclusive(path, b"")?;
        crate::paths::check_private_file(path)?;
        let conn = Connection::open(path).with_context(|| format!("opening {}", path.display()))?;
        Self::init(conn)
    }

    pub fn open_in_memory() -> Result<Self> {
        Self::init(Connection::open_in_memory()?)
    }

    fn init(conn: Connection) -> Result<Self> {
        conn.execute_batch(
            "PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON; PRAGMA secure_delete=ON; \
             PRAGMA busy_timeout=5000;",
        )?;
        conn.execute_batch(SCHEMA)?;
        Ok(Self {
            conn: Arc::new(Mutex::new(conn)),
        })
    }

    pub fn call_sync<R>(&self, f: impl FnOnce(&mut Connection) -> Result<R>) -> Result<R> {
        let mut c = self.conn.lock().unwrap_or_else(|e| e.into_inner());
        f(&mut c)
    }

    pub async fn call<R, F>(&self, f: F) -> Result<R>
    where
        F: FnOnce(&mut Connection) -> Result<R> + Send + 'static,
        R: Send + 'static,
    {
        let this = self.clone();
        tokio::task::spawn_blocking(move || this.call_sync(f)).await?
    }
}

pub fn get_meta(c: &Connection, key: &str) -> Result<Option<String>> {
    Ok(
        c.query_row("SELECT value FROM meta WHERE key=?1", params![key], |r| {
            r.get(0)
        })
        .optional()?,
    )
}

pub fn set_meta(c: &Connection, key: &str, value: &str) -> Result<()> {
    c.execute(
        "INSERT INTO meta (key, value) VALUES (?1, ?2) \
         ON CONFLICT(key) DO UPDATE SET value=excluded.value",
        params![key, value],
    )?;
    Ok(())
}

pub fn insert_workspace(c: &Connection, w: &Workspace) -> Result<()> {
    c.execute(
        "INSERT INTO workspaces (id, name, container_id, tags, default_model, mcp_servers, \
         created_at, updated_at) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)",
        params![
            w.id,
            w.name,
            w.container_id,
            serde_json::to_string(&w.tags)?,
            w.default_model,
            serde_json::to_string(&w.mcp_servers)?,
            w.created_at,
            w.updated_at
        ],
    )?;
    Ok(())
}

pub fn update_workspace(c: &Connection, w: &Workspace) -> Result<()> {
    c.execute(
        "UPDATE workspaces SET name=?2, container_id=?3, tags=?4, default_model=?5, \
         mcp_servers=?6, updated_at=?7 WHERE id=?1",
        params![
            w.id,
            w.name,
            w.container_id,
            serde_json::to_string(&w.tags)?,
            w.default_model,
            serde_json::to_string(&w.mcp_servers)?,
            w.updated_at
        ],
    )?;
    Ok(())
}

pub fn set_cloudflare_agent(c: &Connection, id: &str, info: &CloudflareAgentInfo) -> Result<()> {
    c.execute(
        "UPDATE workspaces SET cf_script=?2, cf_url=?3, cf_deployed_at=?4, updated_at=?4 \
         WHERE id=?1",
        params![id, info.script_name, info.url, info.deployed_at],
    )?;
    Ok(())
}

pub fn get_workspace(c: &Connection, id: &str) -> Result<Option<Workspace>> {
    Ok(c.query_row(
        "SELECT * FROM workspaces WHERE id=?1",
        params![id],
        row_workspace,
    )
    .optional()?)
}

pub fn list_workspaces(
    c: &Connection,
    q: Option<&str>,
    tag: Option<&str>,
) -> Result<Vec<Workspace>> {
    let mut stmt = c.prepare(
        "SELECT * FROM workspaces WHERE (?1 IS NULL OR name LIKE ?1 ESCAPE '\\') \
         AND (?2 IS NULL OR EXISTS (SELECT 1 FROM json_each(workspaces.tags) WHERE value = ?2)) \
         ORDER BY created_at, id",
    )?;
    let rows = stmt.query_map(params![q.map(like_pattern), tag], row_workspace)?;
    Ok(rows.collect::<rusqlite::Result<_>>()?)
}

pub fn delete_workspace(c: &mut Connection, id: &str) -> Result<bool> {
    Ok(c.execute("DELETE FROM workspaces WHERE id=?1", params![id])? > 0)
}

pub fn insert_artifact(c: &Connection, a: &ArtifactMeta) -> Result<()> {
    c.execute(
        "INSERT INTO artifacts (id, workspace_id, name, mime_type, size, tags, created_at) \
         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)",
        params![
            a.id,
            a.workspace_id,
            a.name,
            a.mime_type,
            a.size,
            serde_json::to_string(&a.tags)?,
            a.created_at
        ],
    )?;
    Ok(())
}

pub fn list_artifacts(
    c: &Connection,
    ws: &str,
    q: Option<&str>,
    tag: Option<&str>,
    mime: Option<&str>,
) -> Result<Vec<ArtifactMeta>> {
    let mut stmt = c.prepare(
        "SELECT * FROM artifacts WHERE workspace_id=?1 \
         AND (?2 IS NULL OR name LIKE ?2 ESCAPE '\\') \
         AND (?3 IS NULL OR EXISTS (SELECT 1 FROM json_each(artifacts.tags) WHERE value = ?3)) \
         AND (?4 IS NULL OR mime_type = ?4) \
         ORDER BY created_at, id",
    )?;
    let rows = stmt.query_map(params![ws, q.map(like_pattern), tag, mime], row_artifact)?;
    Ok(rows.collect::<rusqlite::Result<_>>()?)
}

pub fn get_artifact(c: &Connection, ws: &str, id: &str) -> Result<Option<ArtifactMeta>> {
    Ok(c.query_row(
        "SELECT * FROM artifacts WHERE workspace_id=?1 AND id=?2",
        params![ws, id],
        row_artifact,
    )
    .optional()?)
}

pub fn delete_artifact(c: &Connection, ws: &str, id: &str) -> Result<bool> {
    Ok(c.execute(
        "DELETE FROM artifacts WHERE workspace_id=?1 AND id=?2",
        params![ws, id],
    )? > 0)
}

pub fn count_encrypted_items(c: &Connection) -> Result<i64> {
    Ok(c.query_row(
        "SELECT (SELECT COUNT(*) FROM artifacts) + (SELECT COUNT(*) FROM runs)",
        [],
        |r| r.get(0),
    )?)
}

pub fn insert_run(c: &Connection, r: &Run) -> Result<()> {
    c.execute(
        "INSERT INTO runs (id, workspace_id, model, mcp_servers, status, created_at) \
         VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
        params![
            r.id,
            r.workspace_id,
            r.model,
            serde_json::to_string(&r.mcp_servers)?,
            r.status,
            r.created_at
        ],
    )?;
    Ok(())
}

pub fn set_run_started(c: &Connection, id: &str, at: i64) -> Result<()> {
    c.execute(
        "UPDATE runs SET status='running', started_at=?2 WHERE id=?1",
        params![id, at],
    )?;
    Ok(())
}

pub fn set_run_finished(
    c: &Connection,
    id: &str,
    status: &str,
    error: Option<&str>,
    usage: Option<&serde_json::Value>,
    at: i64,
) -> Result<()> {
    c.execute(
        "UPDATE runs SET status=?2, error=?3, usage=?4, finished_at=?5 WHERE id=?1",
        params![
            id,
            status,
            error,
            usage.map(serde_json::Value::to_string),
            at
        ],
    )?;
    Ok(())
}

pub fn get_run(c: &Connection, id: &str) -> Result<Option<Run>> {
    Ok(
        c.query_row("SELECT * FROM runs WHERE id=?1", params![id], row_run)
            .optional()?,
    )
}

pub fn list_runs(c: &Connection, ws: &str) -> Result<Vec<Run>> {
    let mut stmt = c.prepare("SELECT * FROM runs WHERE workspace_id=?1 ORDER BY created_at, id")?;
    let rows = stmt.query_map(params![ws], row_run)?;
    Ok(rows.collect::<rusqlite::Result<_>>()?)
}

pub fn run_ids_for_workspace(c: &Connection, ws: &str) -> Result<Vec<String>> {
    let mut stmt = c.prepare("SELECT id FROM runs WHERE workspace_id=?1")?;
    let rows = stmt.query_map(params![ws], |r| r.get(0))?;
    Ok(rows.collect::<rusqlite::Result<_>>()?)
}

/// Runs left queued/running by a previous process are marked failed.
pub fn mark_interrupted_runs(c: &Connection, at: i64) -> Result<usize> {
    Ok(c.execute(
        "UPDATE runs SET status='failed', error='interrupted: daemon restarted', finished_at=?1 \
         WHERE status IN ('queued', 'running')",
        params![at],
    )?)
}

pub fn insert_mcp_server(c: &Connection, name: &str, def_json: &str, at: i64) -> Result<()> {
    c.execute(
        "INSERT INTO mcp_servers (name, definition, created_at) VALUES (?1, ?2, ?3)",
        params![name, def_json, at],
    )?;
    Ok(())
}

pub fn list_mcp_servers(c: &Connection) -> Result<Vec<(String, String)>> {
    let mut stmt = c.prepare("SELECT name, definition FROM mcp_servers ORDER BY name")?;
    let rows = stmt.query_map([], |r| Ok((r.get(0)?, r.get(1)?)))?;
    Ok(rows.collect::<rusqlite::Result<_>>()?)
}

pub fn delete_mcp_server(c: &Connection, name: &str) -> Result<bool> {
    Ok(c.execute("DELETE FROM mcp_servers WHERE name=?1", params![name])? > 0)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ws(id: &str, name: &str, tags: &[&str]) -> Workspace {
        Workspace {
            id: id.into(),
            name: name.into(),
            container_id: Some("c1".into()),
            tags: tags.iter().map(|s| s.to_string()).collect(),
            default_model: None,
            mcp_servers: vec![],
            cloudflare_agent: None,
            created_at: 1,
            updated_at: 1,
        }
    }

    #[test]
    fn workspace_crud_and_search() {
        let db = Db::open_in_memory().unwrap();
        db.call_sync(|c| {
            insert_workspace(c, &ws("a", "Research 100%", &["x", "y"]))?;
            insert_workspace(c, &ws("b", "Shopping", &["y"]))?;
            assert_eq!(list_workspaces(c, None, None)?.len(), 2);
            assert_eq!(list_workspaces(c, Some("100%"), None)?.len(), 1);
            assert_eq!(list_workspaces(c, Some("_"), None)?.len(), 0);
            assert_eq!(list_workspaces(c, None, Some("y"))?.len(), 2);
            assert_eq!(list_workspaces(c, None, Some("x"))?[0].id, "a");
            let mut w = get_workspace(c, "a")?.unwrap();
            w.name = "Renamed".into();
            update_workspace(c, &w)?;
            assert_eq!(get_workspace(c, "a")?.unwrap().name, "Renamed");
            insert_artifact(
                c,
                &ArtifactMeta {
                    id: "art".into(),
                    workspace_id: "a".into(),
                    name: "notes.md".into(),
                    mime_type: "text/markdown".into(),
                    size: 3,
                    tags: vec!["draft".into()],
                    created_at: 2,
                },
            )?;
            assert_eq!(
                list_artifacts(c, "a", Some("notes"), Some("draft"), None)?.len(),
                1
            );
            assert_eq!(
                list_artifacts(c, "a", None, None, Some("image/png"))?.len(),
                0
            );
            assert!(get_artifact(c, "b", "art")?.is_none());
            assert_eq!(count_encrypted_items(c)?, 1);
            assert!(delete_workspace(c, "a")?);
            assert_eq!(count_encrypted_items(c)?, 0);
            Ok(())
        })
        .unwrap();
    }
}
