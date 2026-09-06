//! Immutable catalog-driven secondary index over a durable relational snapshot.
//!
//! The index borrows the relational engine immutably for its full lifetime. Rust therefore prevents
//! a caller from mutating that engine while the index is live, making the snapshot semantics
//! explicit instead of silently serving stale post-write results. Rebuilding after reopen derives
//! the same index from replayed durable state; no new on-disk format or persistence boundary is used.

use std::collections::{BTreeMap, BTreeSet};

use db_core::{DbError, Result};

use crate::query::{CompareOp, Projection, QueryResult};
use crate::relational::{Cell, ColumnType, RelationalEngine};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum IndexKeyType {
    Int64,
    Text,
}

impl IndexKeyType {
    fn from_column_type(ty: &ColumnType) -> Self {
        match ty {
            ColumnType::Int64 => Self::Int64,
            ColumnType::Text => Self::Text,
        }
    }

    fn accepts(self, value: &Cell) -> bool {
        matches!(
            (self, value),
            (Self::Int64, Cell::Int64(_)) | (Self::Text, Cell::Text(_))
        )
    }
}

/// Read-only secondary index tied to one immutable relational-engine snapshot.
///
/// Entries map one typed column value to all matching rows. Query results are restored to primary-
/// key order so indexed equality and range predicates have the same deterministic ordering as scans.
pub struct SecondaryIndex<'a> {
    _engine: &'a RelationalEngine,
    table: String,
    column: String,
    key_type: IndexKeyType,
    primary_key: usize,
    columns: Vec<String>,
    entries: BTreeMap<Cell, Vec<Vec<Cell>>>,
}

impl std::fmt::Debug for SecondaryIndex<'_> {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("SecondaryIndex")
            .field("table", &self.table)
            .field("column", &self.column)
            .field("distinct_keys", &self.entries.len())
            .finish_non_exhaustive()
    }
}

impl<'a> SecondaryIndex<'a> {
    /// Builds a secondary index from the replayed durable state of `table`.
    pub fn build(engine: &'a RelationalEngine, table: &str, column: &str) -> Result<Self> {
        let schema = engine.schema(table)?;
        let column_index = schema
            .columns
            .iter()
            .position(|candidate| candidate.name == column)
            .ok_or_else(|| DbError::InvalidInput(format!("unknown column {column}")))?;
        let key_type = IndexKeyType::from_column_type(&schema.columns[column_index].ty);
        let columns = schema
            .columns
            .iter()
            .map(|candidate| candidate.name.clone())
            .collect();
        let mut entries: BTreeMap<Cell, Vec<Vec<Cell>>> = BTreeMap::new();
        for (_, row) in engine.rows(table)? {
            entries
                .entry(row[column_index].clone())
                .or_default()
                .push(row.to_vec());
        }
        Ok(Self {
            _engine: engine,
            table: table.to_owned(),
            column: column.to_owned(),
            key_type,
            primary_key: schema.primary_key,
            columns,
            entries,
        })
    }

    /// Indexed table name.
    #[must_use]
    pub fn table(&self) -> &str {
        &self.table
    }

    /// Indexed column name.
    #[must_use]
    pub fn column(&self) -> &str {
        &self.column
    }

    /// Executes any supported typed comparison through this index.
    pub fn execute_compare(
        &self,
        op: CompareOp,
        value: &Cell,
        projection: &Projection,
    ) -> Result<QueryResult> {
        if !self.key_type.accepts(value) {
            return Err(DbError::InvalidInput(
                "index lookup literal type does not match indexed column type".to_owned(),
            ));
        }
        let projection = self.resolve_projection(projection)?;
        let columns = projection
            .iter()
            .map(|index| self.columns[*index].clone())
            .collect();
        let mut matching = self
            .entries
            .iter()
            .filter(|(key, _)| match op {
                CompareOp::Eq => *key == value,
                CompareOp::Lt => *key < value,
                CompareOp::Le => *key <= value,
                CompareOp::Gt => *key > value,
                CompareOp::Ge => *key >= value,
            })
            .flat_map(|(_, rows)| rows.iter())
            .collect::<Vec<_>>();
        matching.sort_by(|left, right| left[self.primary_key].cmp(&right[self.primary_key]));
        let rows = matching
            .into_iter()
            .map(|row| projection.iter().map(|index| row[*index].clone()).collect())
            .collect();
        Ok(QueryResult { columns, rows })
    }

    /// Executes an equality lookup through this index.
    pub fn execute_eq(&self, value: &Cell, projection: &Projection) -> Result<QueryResult> {
        self.execute_compare(CompareOp::Eq, value, projection)
    }

    fn resolve_projection(&self, projection: &Projection) -> Result<Vec<usize>> {
        match projection {
            Projection::All => Ok((0..self.columns.len()).collect()),
            Projection::Columns(columns) => {
                if columns.is_empty() {
                    return Err(DbError::InvalidInput(
                        "query projection must contain at least one column".to_owned(),
                    ));
                }
                let mut seen = BTreeSet::new();
                columns
                    .iter()
                    .map(|name| {
                        if !seen.insert(name.as_str()) {
                            return Err(DbError::InvalidInput(format!(
                                "duplicate projected column {name}"
                            )));
                        }
                        self.columns
                            .iter()
                            .position(|candidate| candidate == name)
                            .ok_or_else(|| DbError::InvalidInput(format!("unknown column {name}")))
                    })
                    .collect()
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::query::{self, Predicate, Query};
    use crate::relational::{Column, RelOp, Schema};
    use tempfile::tempdir;

    fn users_schema() -> Schema {
        Schema {
            columns: vec![
                Column {
                    name: "id".to_owned(),
                    ty: ColumnType::Int64,
                },
                Column {
                    name: "team".to_owned(),
                    ty: ColumnType::Text,
                },
                Column {
                    name: "name".to_owned(),
                    ty: ColumnType::Text,
                },
            ],
            primary_key: 0,
        }
    }

    fn seed(engine: &mut RelationalEngine) -> Result<()> {
        engine.commit(&[
            RelOp::CreateTable {
                name: "users".to_owned(),
                schema: users_schema(),
            },
            RelOp::UpsertRow {
                table: "users".to_owned(),
                row: vec![
                    Cell::Int64(3),
                    Cell::Text("systems".to_owned()),
                    Cell::Text("Edsger".to_owned()),
                ],
            },
            RelOp::UpsertRow {
                table: "users".to_owned(),
                row: vec![
                    Cell::Int64(1),
                    Cell::Text("languages".to_owned()),
                    Cell::Text("Ada".to_owned()),
                ],
            },
            RelOp::UpsertRow {
                table: "users".to_owned(),
                row: vec![
                    Cell::Int64(2),
                    Cell::Text("systems".to_owned()),
                    Cell::Text("Grace".to_owned()),
                ],
            },
            RelOp::UpsertRow {
                table: "users".to_owned(),
                row: vec![
                    Cell::Int64(4),
                    Cell::Text("theory".to_owned()),
                    Cell::Text("Donald".to_owned()),
                ],
            },
        ])?;
        Ok(())
    }

    #[test]
    fn every_indexed_comparison_matches_scan_and_primary_key_order() -> Result<()> {
        let dir = tempdir()?;
        let path = dir.path().join("range-index.log");
        let mut engine = RelationalEngine::open(&path)?;
        seed(&mut engine)?;
        let index = SecondaryIndex::build(&engine, "users", "team")?;
        for op in [
            CompareOp::Eq,
            CompareOp::Lt,
            CompareOp::Le,
            CompareOp::Gt,
            CompareOp::Ge,
        ] {
            let value = Cell::Text("systems".to_owned());
            let projection = Projection::Columns(vec!["id".to_owned(), "name".to_owned()]);
            let indexed = index.execute_compare(op, &value, &projection)?;
            let scanned = query::execute(
                &engine,
                &Query {
                    table: "users".to_owned(),
                    predicate: Some(Predicate {
                        column: "team".to_owned(),
                        op,
                        value: value.clone(),
                    }),
                    projection,
                },
            )?;
            assert_eq!(indexed, scanned);
        }
        Ok(())
    }

    #[test]
    fn rebuilt_range_index_after_reopen_matches_scan_oracle() -> Result<()> {
        let dir = tempdir()?;
        let path = dir.path().join("range-reopen.log");
        let mut engine = RelationalEngine::open(&path)?;
        seed(&mut engine)?;
        engine.commit(&[RelOp::DeleteRow {
            table: "users".to_owned(),
            key: Cell::Int64(3),
        }])?;
        drop(engine);
        let engine = RelationalEngine::open(&path)?;
        let index = SecondaryIndex::build(&engine, "users", "team")?;
        let value = Cell::Text("systems".to_owned());
        let query = Query {
            table: "users".to_owned(),
            predicate: Some(Predicate {
                column: "team".to_owned(),
                op: CompareOp::Ge,
                value: value.clone(),
            }),
            projection: Projection::All,
        };
        assert_eq!(
            index.execute_compare(CompareOp::Ge, &value, &Projection::All)?,
            query::execute(&engine, &query)?
        );
        Ok(())
    }

    #[test]
    fn index_validation_rejects_wrong_types_and_bad_projection() -> Result<()> {
        let dir = tempdir()?;
        let path = dir.path().join("index-validation.log");
        let mut engine = RelationalEngine::open(&path)?;
        seed(&mut engine)?;
        let index = SecondaryIndex::build(&engine, "users", "team")?;
        assert!(index
            .execute_compare(CompareOp::Gt, &Cell::Int64(7), &Projection::All)
            .is_err());
        assert!(index
            .execute_eq(
                &Cell::Text("systems".to_owned()),
                &Projection::Columns(vec!["missing".to_owned()]),
            )
            .is_err());
        Ok(())
    }
}
