//! Bounded single-store development snapshots; this is not Qt's portable backup format.
use rusqlite::{
    Connection,
    backup::{Backup, StepResult},
};

pub const MAX_BYTES: usize = 16 * 1024 * 1024;
pub const SET_FORMAT: &str = "sentinel-dev-sqlite-set-v1";
pub const SET_MAGIC: &[u8] = b"SENTINEL-DEV-SET-1\n";
const DOMAINS: [&str; 4] = ["chat", "settings", "memory", "agent"];

/// Caller holds the state lock while collecting all four owner snapshots.
#[derive(Default)]
pub struct SnapshotSet {
    stores: Vec<Vec<u8>>,
    bytes: usize,
}
impl SnapshotSet {
    pub fn push(&mut self, domain: &str, data: Vec<u8>) -> rusqlite::Result<()> {
        if DOMAINS.get(self.stores.len()) != Some(&domain)
            || data.is_empty()
            || self
                .bytes
                .checked_add(data.len())
                .is_none_or(|n| n > MAX_BYTES - 4096 - SET_MAGIC.len() - 4)
        {
            return Err(rusqlite::Error::InvalidQuery);
        }
        self.bytes += data.len();
        self.stores.push(data);
        Ok(())
    }
    pub fn finish(self) -> rusqlite::Result<Vec<u8>> {
        use sha2::{Digest, Sha256};
        if self.stores.len() != DOMAINS.len() {
            return Err(rusqlite::Error::InvalidQuery);
        }
        let entries: Vec<_> = DOMAINS.iter().zip(&self.stores).map(|(domain,bytes)| serde_json::json!({"domain":domain,"size":bytes.len(),"sha256":format!("{:x}",Sha256::digest(bytes))})).collect();
        let manifest =
            serde_json::to_vec(&serde_json::json!({"format":SET_FORMAT,"stores":entries}))
                .map_err(|_| rusqlite::Error::InvalidQuery)?;
        if manifest.len() > 4096 {
            return Err(rusqlite::Error::InvalidQuery);
        }
        let mut output = Vec::with_capacity(SET_MAGIC.len() + 4 + manifest.len() + self.bytes);
        output.extend_from_slice(SET_MAGIC);
        output.extend_from_slice(&(manifest.len() as u32).to_be_bytes());
        output.extend_from_slice(&manifest);
        for bytes in self.stores {
            output.extend_from_slice(&bytes);
        }
        Ok(output)
    }
}

/// Caller must hold the owning store's state lock and reject active mutations.
pub fn snapshot(source: &Connection) -> rusqlite::Result<Vec<u8>> {
    let page_size: usize = source.query_row("PRAGMA page_size", [], |r| r.get(0))?;
    let pages: usize = source.query_row("PRAGMA page_count", [], |r| r.get(0))?;
    if !(512..=65536).contains(&page_size)
        || pages
            .checked_mul(page_size)
            .is_none_or(|size| size > MAX_BYTES)
    {
        return Err(rusqlite::Error::InvalidQuery);
    }
    let mut destination = Connection::open_in_memory()?;
    {
        let backup = Backup::new(source, &mut destination)?;
        let mut steps = 0usize;
        loop {
            steps += 1;
            if steps > 4096 {
                return Err(rusqlite::Error::InvalidQuery);
            }
            let result = backup.step(16)?;
            let progress = backup.progress();
            if progress.pagecount < 0
                || (progress.pagecount as usize)
                    .checked_mul(page_size)
                    .is_none_or(|size| size > MAX_BYTES)
            {
                return Err(rusqlite::Error::InvalidQuery);
            }
            match result {
                StepResult::Done => break,
                StepResult::More => continue,
                // Never spin or sleep while the owning daemon state is locked.
                _ => return Err(rusqlite::Error::InvalidQuery),
            }
        }
    }
    let data = destination.serialize("main")?;
    if data.is_empty() || data.len() > MAX_BYTES {
        return Err(rusqlite::Error::InvalidQuery);
    }
    Ok(data.to_vec())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn snapshot_is_independent_and_contains_committed_data() {
        let source = Connection::open_in_memory().unwrap();
        source
            .execute_batch("CREATE TABLE item(value TEXT); INSERT INTO item VALUES('before');")
            .unwrap();
        let bytes = snapshot(&source).unwrap();
        source.execute("UPDATE item SET value='after'", []).unwrap();
        let mut copy = Connection::open_in_memory().unwrap();
        copy.deserialize_read_exact("main", &mut bytes.as_slice(), bytes.len(), true)
            .unwrap();
        let value: String = copy
            .query_row("SELECT value FROM item", [], |r| r.get(0))
            .unwrap();
        assert_eq!(value, "before");
    }
    #[test]
    fn oversized_database_is_refused() {
        let source = Connection::open_in_memory().unwrap();
        source
            .execute_batch(
                "CREATE TABLE item(value BLOB); INSERT INTO item VALUES(zeroblob(17000000));",
            )
            .unwrap();
        assert!(snapshot(&source).is_err());
    }
}
