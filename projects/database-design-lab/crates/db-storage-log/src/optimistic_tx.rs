//! Single-process optimistic concurrency control for durable relational transactions.
//!
//! Transaction bodies run outside the durable commit mutex from an explicit row read-set snapshot.
//! Commit reacquires the mutex, validates every observed row version/value, then delegates successful
//! publication to [`RelationalEngine::commit`]. The existing one-record append + `sync_data`
//! durability boundary is unchanged.

use std::collections::{BTreeMap, BTreeSet};
use std::path::Path;
use std::sync::{Arc, Mutex, MutexGuard};

use db_core::{DbError, Result};

use crate::relational::{Cell, RelOp, RelationalEngine, Schema};

/// Stable prefix used for optimistic validation conflicts.
pub const OPTIMISTIC_CONFLICT_PREFIX: &str = "optimistic transaction conflict";

/// One explicitly declared row read in an optimistic transaction.
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord)]
pub struct RowRead {
    /// Table containing the row.
    pub table: String,
    /// Primary-key value identifying the row.
    pub key: Cell,
}

impl RowRead {
    /// Creates one declared row read.
    #[must_use]
    pub fn new(table: impl Into<String>, key: Cell) -> Self {
        Self {
            table: table.into(),
            key,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct ObservedRow {
    row: Option<Vec<Cell>>,
    version: u64,
}

/// Immutable snapshot supplied to an optimistic transaction body.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct OptimisticSnapshot {
    rows: BTreeMap<RowRead, ObservedRow>,
}

impl OptimisticSnapshot {
    /// Returns a declared row exactly as observed while the snapshot mutex was held.
    ///
    /// Access to undeclared rows is rejected so the commit validator has a complete read set.
    pub fn row(&self, read: &RowRead) -> Result<Option<&[Cell]>> {
        let observed = self.rows.get(read).ok_or_else(|| {
            DbError::InvalidInput(format!(
                "optimistic transaction attempted undeclared read on table {}",
                read.table
            ))
        })?;
        Ok(observed.row.as_deref())
    }
}

#[derive(Debug)]
struct VersionedRelationalState {
    engine: RelationalEngine,
    row_versions: BTreeMap<RowRead, u64>,
}

/// Cloneable single-process optimistic concurrency wrapper around [`RelationalEngine`].
///
/// Existing rows at open form version zero. Every successful wrapper commit stamps each touched row
/// with the durable relational transaction id, which detects ABA changes as well as value changes
/// during the lifetime of this process.
#[derive(Debug, Clone)]
pub struct OptimisticRelationalEngine {
    inner: Arc<Mutex<VersionedRelationalState>>,
}

impl OptimisticRelationalEngine {
    /// Opens or creates the underlying durable relational database.
    pub fn open(path: impl AsRef<Path>) -> Result<Self> {
        Ok(Self {
            inner: Arc::new(Mutex::new(VersionedRelationalState {
                engine: RelationalEngine::open(path)?,
                row_versions: BTreeMap::new(),
            })),
        })
    }

    fn lock(&self) -> Result<MutexGuard<'_, VersionedRelationalState>> {
        self.inner.lock().map_err(|_| DbError::Poisoned)
    }

    /// Returns a cloned committed row for non-transactional inspection.
    pub fn row(&self, table: &str, key: &Cell) -> Result<Option<Vec<Cell>>> {
        Ok(self.lock()?.engine.row(table, key)?.map(<[Cell]>::to_vec))
    }

    /// Returns the transaction id the next successful durable relational commit will receive.
    pub fn next_transaction_id(&self) -> Result<u64> {
        Ok(self.lock()?.engine.next_transaction_id())
    }

    /// Executes one optimistic transaction over an explicit row read set.
    ///
    /// The snapshot is captured while holding the process-local mutex, but `body` runs after that
    /// mutex is released. On return, every observed row is revalidated before any durable append.
    /// A conflict returns [`DbError::InvalidInput`] whose message starts with
    /// [`OPTIMISTIC_CONFLICT_PREFIX`], and no transaction record is appended or published.
    /// Successful writes retain the existing relational one-record append + sync commit boundary.
    pub fn transaction<T, F>(&self, reads: &[RowRead], body: F) -> Result<(Option<u64>, T)>
    where
        F: FnOnce(&OptimisticSnapshot) -> Result<(T, Vec<RelOp>)>,
    {
        let snapshot = self.snapshot(reads)?;
        let (output, ops) = body(&snapshot)?;

        let mut state = self.lock()?;
        Self::validate_snapshot(&state, &snapshot)?;
        if ops.is_empty() {
            return Ok((None, output));
        }

        let touched = touched_rows(&state.engine, &ops);
        let tx_id = state.engine.commit(&ops)?;
        for row in touched {
            state.row_versions.insert(row, tx_id);
        }
        Ok((Some(tx_id), output))
    }

    fn snapshot(&self, reads: &[RowRead]) -> Result<OptimisticSnapshot> {
        let state = self.lock()?;
        let mut unique = BTreeSet::new();
        let mut rows = BTreeMap::new();
        for read in reads {
            if !unique.insert(read.clone()) {
                return Err(DbError::InvalidInput(format!(
                    "duplicate optimistic row read on table {}",
                    read.table
                )));
            }
            let row = state
                .engine
                .row(&read.table, &read.key)?
                .map(<[Cell]>::to_vec);
            let version = state.row_versions.get(read).copied().unwrap_or(0);
            rows.insert(read.clone(), ObservedRow { row, version });
        }
        Ok(OptimisticSnapshot { rows })
    }

    fn validate_snapshot(
        state: &VersionedRelationalState,
        snapshot: &OptimisticSnapshot,
    ) -> Result<()> {
        for (read, observed) in &snapshot.rows {
            let current = state
                .engine
                .row(&read.table, &read.key)?
                .map(<[Cell]>::to_vec);
            let current_version = state.row_versions.get(read).copied().unwrap_or(0);
            if current != observed.row || current_version != observed.version {
                return Err(DbError::InvalidInput(format!(
                    "{OPTIMISTIC_CONFLICT_PREFIX} on table {}",
                    read.table
                )));
            }
        }
        Ok(())
    }
}

fn touched_rows(engine: &RelationalEngine, ops: &[RelOp]) -> Vec<RowRead> {
    let mut schemas = engine
        .catalog()
        .map(|(name, schema)| (name.to_owned(), schema.clone()))
        .collect::<BTreeMap<String, Schema>>();
    let mut touched = Vec::new();

    for op in ops {
        match op {
            RelOp::CreateTable { name, schema } => {
                schemas.insert(name.clone(), schema.clone());
            }
            RelOp::UpsertRow { table, row } => {
                if let Some(schema) = schemas.get(table) {
                    if let Some(key) = row.get(schema.primary_key) {
                        touched.push(RowRead::new(table.clone(), key.clone()));
                    }
                }
            }
            RelOp::DeleteRow { table, key } => {
                touched.push(RowRead::new(table.clone(), key.clone()));
            }
        }
    }
    touched
}

#[cfg(test)]
mod tests {
    use std::sync::{Arc, Barrier};
    use std::thread;

    use tempfile::tempdir;

    use super::*;
    use crate::relational::{Column, ColumnType};

    fn account_schema() -> Schema {
        Schema {
            columns: vec![
                Column {
                    name: "id".to_owned(),
                    ty: ColumnType::Int64,
                },
                Column {
                    name: "balance".to_owned(),
                    ty: ColumnType::Int64,
                },
            ],
            primary_key: 0,
        }
    }

    fn seed(path: &Path, rows: &[(i64, i64)]) -> Result<()> {
        let mut engine = RelationalEngine::open(path)?;
        let mut ops = vec![RelOp::CreateTable {
            name: "accounts".to_owned(),
            schema: account_schema(),
        }];
        ops.extend(rows.iter().map(|(id, balance)| RelOp::UpsertRow {
            table: "accounts".to_owned(),
            row: vec![Cell::Int64(*id), Cell::Int64(*balance)],
        }));
        engine.commit(&ops)?;
        Ok(())
    }

    fn increment_from_snapshot(snapshot: &OptimisticSnapshot, read: &RowRead) -> Result<RelOp> {
        let row = snapshot
            .row(read)?
            .ok_or_else(|| DbError::InvalidInput("expected seeded account row".to_owned()))?;
        let Cell::Int64(balance) = &row[1] else {
            return Err(DbError::InvalidInput(
                "expected integer account balance".to_owned(),
            ));
        };
        Ok(RelOp::UpsertRow {
            table: read.table.clone(),
            row: vec![read.key.clone(), Cell::Int64(balance + 1)],
        })
    }

    #[test]
    fn concurrent_lost_update_is_rejected_before_durable_append() -> Result<()> {
        let dir = tempdir()?;
        let path = dir.path().join("occ.log");
        seed(&path, &[(1, 0)])?;

        let engine = OptimisticRelationalEngine::open(&path)?;
        assert_eq!(engine.next_transaction_id()?, 2);
        let barrier = Arc::new(Barrier::new(2));
        let read = RowRead::new("accounts", Cell::Int64(1));

        let mut handles = Vec::new();
        for _ in 0..2 {
            let worker = engine.clone();
            let worker_barrier = Arc::clone(&barrier);
            let worker_read = read.clone();
            handles.push(thread::spawn(move || {
                worker.transaction(std::slice::from_ref(&worker_read), |snapshot| {
                    let op = increment_from_snapshot(snapshot, &worker_read)?;
                    worker_barrier.wait();
                    Ok(((), vec![op]))
                })
            }));
        }

        let results = handles
            .into_iter()
            .map(|handle| handle.join().expect("worker must not panic"))
            .collect::<Vec<_>>();
        assert_eq!(results.iter().filter(|result| result.is_ok()).count(), 1);
        assert_eq!(
            results
                .iter()
                .filter(|result| matches!(
                    result,
                    Err(DbError::InvalidInput(message))
                        if message.starts_with(OPTIMISTIC_CONFLICT_PREFIX)
                ))
                .count(),
            1
        );
        assert_eq!(engine.next_transaction_id()?, 3);
        assert_eq!(
            engine.row("accounts", &Cell::Int64(1))?,
            Some(vec![Cell::Int64(1), Cell::Int64(1)])
        );

        drop(engine);
        let reopened = RelationalEngine::open(&path)?;
        assert_eq!(reopened.next_transaction_id(), 3);
        assert_eq!(
            reopened.row("accounts", &Cell::Int64(1))?,
            Some(&[Cell::Int64(1), Cell::Int64(1)][..])
        );
        Ok(())
    }

    #[test]
    fn disjoint_transaction_bodies_overlap_and_both_reopen() -> Result<()> {
        let dir = tempdir()?;
        let path = dir.path().join("occ-disjoint.log");
        seed(&path, &[(1, 10), (2, 20)])?;

        let engine = OptimisticRelationalEngine::open(&path)?;
        let barrier = Arc::new(Barrier::new(2));
        let reads = [
            RowRead::new("accounts", Cell::Int64(1)),
            RowRead::new("accounts", Cell::Int64(2)),
        ];

        let handles = reads
            .into_iter()
            .map(|read| {
                let worker = engine.clone();
                let worker_barrier = Arc::clone(&barrier);
                thread::spawn(move || {
                    worker.transaction(std::slice::from_ref(&read), |snapshot| {
                        let op = increment_from_snapshot(snapshot, &read)?;
                        worker_barrier.wait();
                        Ok(((), vec![op]))
                    })
                })
            })
            .collect::<Vec<_>>();

        let mut tx_ids = handles
            .into_iter()
            .map(|handle| handle.join().expect("worker must not panic"))
            .collect::<Result<Vec<_>>>()?
            .into_iter()
            .map(|(tx_id, ())| tx_id.expect("each worker writes"))
            .collect::<Vec<_>>();
        tx_ids.sort_unstable();
        assert_eq!(tx_ids, vec![2, 3]);
        assert_eq!(engine.next_transaction_id()?, 4);

        drop(engine);
        let reopened = RelationalEngine::open(&path)?;
        assert_eq!(
            reopened.row("accounts", &Cell::Int64(1))?,
            Some(&[Cell::Int64(1), Cell::Int64(11)][..])
        );
        assert_eq!(
            reopened.row("accounts", &Cell::Int64(2))?,
            Some(&[Cell::Int64(2), Cell::Int64(21)][..])
        );
        assert_eq!(reopened.next_transaction_id(), 4);
        Ok(())
    }

    #[test]
    fn undeclared_snapshot_reads_fail_before_commit() -> Result<()> {
        let dir = tempdir()?;
        let path = dir.path().join("occ-undeclared.log");
        seed(&path, &[(1, 10), (2, 20)])?;
        let engine = OptimisticRelationalEngine::open(&path)?;
        let declared = RowRead::new("accounts", Cell::Int64(1));
        let undeclared = RowRead::new("accounts", Cell::Int64(2));

        let result = engine.transaction(std::slice::from_ref(&declared), |snapshot| {
            let _ = snapshot.row(&undeclared)?;
            Ok(((), Vec::new()))
        });
        assert!(matches!(result, Err(DbError::InvalidInput(_))));
        assert_eq!(engine.next_transaction_id()?, 2);
        Ok(())
    }
}
