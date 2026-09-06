//! Transaction-maintained secondary indexes over the durable relational engine.
//!
//! Index definitions are process-local. Durable relational commits still use the unchanged v1
//! transaction record and append/sync boundary. Successful commits rebuild indexes from the exact
//! committed state; failed commits leave both durable state and indexes unchanged.

use std::collections::{BTreeMap, BTreeSet};
use std::path::Path;

use db_core::{DbError, Result};

use crate::query::{self, CompareOp, ConjunctiveQuery, Projection, Query, QueryResult};
use crate::relational::{Cell, ColumnType, RelOp, RelationalEngine};

#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord)]
struct IndexSpec {
    table: String,
    column: String,
}

#[derive(Debug, Clone)]
struct MaterializedIndex {
    key_type: ColumnType,
    primary_key: usize,
    columns: Vec<String>,
    entries: BTreeMap<Cell, Vec<Vec<Cell>>>,
}

impl MaterializedIndex {
    fn build(engine: &RelationalEngine, spec: IndexSpec) -> Result<Self> {
        let schema = engine.schema(&spec.table)?;
        let column_index = schema
            .columns
            .iter()
            .position(|candidate| candidate.name == spec.column)
            .ok_or_else(|| DbError::InvalidInput(format!("unknown column {}", spec.column)))?;
        let key_type = schema.columns[column_index].ty.clone();
        let columns = schema
            .columns
            .iter()
            .map(|column| column.name.clone())
            .collect();
        let mut entries: BTreeMap<Cell, Vec<Vec<Cell>>> = BTreeMap::new();
        for (_, row) in engine.rows(&spec.table)? {
            entries
                .entry(row[column_index].clone())
                .or_default()
                .push(row.to_vec());
        }
        Ok(Self {
            key_type,
            primary_key: schema.primary_key,
            columns,
            entries,
        })
    }

    fn matching_rows(&self, op: CompareOp, value: &Cell) -> Result<Vec<&[Cell]>> {
        let type_matches = matches!(
            (&self.key_type, value),
            (ColumnType::Int64, Cell::Int64(_)) | (ColumnType::Text, Cell::Text(_))
        );
        if !type_matches {
            return Err(DbError::InvalidInput(
                "index lookup literal type does not match indexed column type".to_owned(),
            ));
        }
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
            .flat_map(|(_, rows)| rows.iter().map(Vec::as_slice))
            .collect::<Vec<_>>();
        matching.sort_by(|left, right| left[self.primary_key].cmp(&right[self.primary_key]));
        Ok(matching)
    }

    fn execute_compare(
        &self,
        op: CompareOp,
        value: &Cell,
        projection: &Projection,
    ) -> Result<QueryResult> {
        let projection = self.resolve_projection(projection)?;
        let columns = projection
            .iter()
            .map(|index| self.columns[*index].clone())
            .collect();
        let rows = self
            .matching_rows(op, value)?
            .into_iter()
            .map(|row| query::project_row(row, &projection))
            .collect();
        Ok(QueryResult { columns, rows })
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

/// Durable relational engine with process-local secondary indexes maintained across commits.
#[derive(Debug)]
pub struct MaintainedIndexEngine {
    engine: RelationalEngine,
    indexes: BTreeMap<IndexSpec, MaterializedIndex>,
}

impl MaintainedIndexEngine {
    /// Opens or creates a durable relational database with no registered process-local indexes.
    pub fn open(path: impl AsRef<Path>) -> Result<Self> {
        Ok(Self {
            engine: RelationalEngine::open(path)?,
            indexes: BTreeMap::new(),
        })
    }

    /// Registers and materializes one catalog-validated secondary index.
    pub fn register_index(&mut self, table: &str, column: &str) -> Result<()> {
        let spec = IndexSpec {
            table: table.to_owned(),
            column: column.to_owned(),
        };
        if self.indexes.contains_key(&spec) {
            return Err(DbError::InvalidInput(format!(
                "secondary index already registered for {table}.{column}"
            )));
        }
        let index = MaterializedIndex::build(&self.engine, spec.clone())?;
        self.indexes.insert(spec, index);
        Ok(())
    }

    /// Atomically commits relational operations through the existing durable boundary and refreshes
    /// all registered indexes from the committed state before returning success.
    pub fn commit(&mut self, ops: &[RelOp]) -> Result<u64> {
        let tx_id = self.engine.commit(ops)?;
        self.rebuild_indexes()?;
        Ok(tx_id)
    }

    /// Executes any matching typed predicate through a registered index, otherwise scans.
    pub fn execute(&self, query: &Query) -> Result<QueryResult> {
        if let Some(predicate) = query.predicate.as_ref() {
            let spec = IndexSpec {
                table: query.table.clone(),
                column: predicate.column.clone(),
            };
            if let Some(index) = self.indexes.get(&spec) {
                return index.execute_compare(predicate.op, &predicate.value, &query.projection);
            }
        }
        query::execute(&self.engine, query)
    }

    /// Executes an AND-conjunction by using the first matching registered index as a candidate driver
    /// and evaluating every predicate against the complete candidate row. Without an applicable index,
    /// execution falls back to the scan oracle.
    pub fn execute_conjunctive(&self, conjunctive: &ConjunctiveQuery) -> Result<QueryResult> {
        let schema = self.engine.schema(&conjunctive.table)?;
        let (projection, predicates) =
            query::prepare_conjunctive(schema, &conjunctive.predicates, &conjunctive.projection)?;
        let driver = predicates.iter().find_map(|(_, predicate)| {
            let spec = IndexSpec {
                table: conjunctive.table.clone(),
                column: predicate.column.clone(),
            };
            self.indexes.get(&spec).map(|index| (index, *predicate))
        });
        let Some((index, driver_predicate)) = driver else {
            return query::execute_conjunctive(&self.engine, conjunctive);
        };
        let columns = projection
            .iter()
            .map(|column| schema.columns[*column].name.clone())
            .collect();
        let rows = index
            .matching_rows(driver_predicate.op, &driver_predicate.value)?
            .into_iter()
            .filter(|row| {
                predicates
                    .iter()
                    .all(|(column, predicate)| query::matches_predicate(&row[*column], predicate))
            })
            .map(|row| query::project_row(row, &projection))
            .collect();
        Ok(QueryResult { columns, rows })
    }

    /// Access to the underlying durable relational state for catalog/read-only inspection.
    #[must_use]
    pub fn relational(&self) -> &RelationalEngine {
        &self.engine
    }

    fn rebuild_indexes(&mut self) -> Result<()> {
        let specs = self.indexes.keys().cloned().collect::<Vec<_>>();
        let mut rebuilt = BTreeMap::new();
        for spec in specs {
            rebuilt.insert(spec.clone(), MaterializedIndex::build(&self.engine, spec)?);
        }
        self.indexes = rebuilt;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::query::{Predicate, Projection};
    use crate::relational::{Column, Schema};
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

    fn seed(engine: &mut MaintainedIndexEngine) -> Result<()> {
        engine.commit(&[
            RelOp::CreateTable {
                name: "users".to_owned(),
                schema: users_schema(),
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
                    Cell::Int64(3),
                    Cell::Text("systems".to_owned()),
                    Cell::Text("Edsger".to_owned()),
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
        engine.register_index("users", "team")?;
        Ok(())
    }

    fn team_query(op: CompareOp) -> Query {
        Query {
            table: "users".to_owned(),
            predicate: Some(Predicate {
                column: "team".to_owned(),
                op,
                value: Cell::Text("systems".to_owned()),
            }),
            projection: Projection::Columns(vec!["id".to_owned(), "name".to_owned()]),
        }
    }

    fn conjunctive_query() -> ConjunctiveQuery {
        ConjunctiveQuery {
            table: "users".to_owned(),
            predicates: vec![
                Predicate {
                    column: "team".to_owned(),
                    op: CompareOp::Ge,
                    value: Cell::Text("systems".to_owned()),
                },
                Predicate {
                    column: "id".to_owned(),
                    op: CompareOp::Le,
                    value: Cell::Int64(3),
                },
            ],
            projection: Projection::Columns(vec!["id".to_owned(), "name".to_owned()]),
        }
    }

    #[test]
    fn maintained_range_indexes_match_scan_after_mutation() -> Result<()> {
        let dir = tempdir()?;
        let path = dir.path().join("maintained-range.log");
        let mut engine = MaintainedIndexEngine::open(&path)?;
        seed(&mut engine)?;
        engine.commit(&[
            RelOp::UpsertRow {
                table: "users".to_owned(),
                row: vec![
                    Cell::Int64(2),
                    Cell::Text("theory".to_owned()),
                    Cell::Text("Grace".to_owned()),
                ],
            },
            RelOp::DeleteRow {
                table: "users".to_owned(),
                key: Cell::Int64(1),
            },
            RelOp::UpsertRow {
                table: "users".to_owned(),
                row: vec![
                    Cell::Int64(0),
                    Cell::Text("systems".to_owned()),
                    Cell::Text("Barbara".to_owned()),
                ],
            },
        ])?;
        for op in [
            CompareOp::Eq,
            CompareOp::Lt,
            CompareOp::Le,
            CompareOp::Gt,
            CompareOp::Ge,
        ] {
            let query = team_query(op);
            assert_eq!(
                engine.execute(&query)?,
                query::execute(engine.relational(), &query)?
            );
        }
        Ok(())
    }

    #[test]
    fn indexed_conjunction_matches_scan_oracle_and_primary_key_order() -> Result<()> {
        let dir = tempdir()?;
        let path = dir.path().join("conjunctive-index.log");
        let mut engine = MaintainedIndexEngine::open(&path)?;
        seed(&mut engine)?;
        let conjunctive = conjunctive_query();
        let indexed = engine.execute_conjunctive(&conjunctive)?;
        let scanned = query::execute_conjunctive(engine.relational(), &conjunctive)?;
        assert_eq!(indexed, scanned);
        assert_eq!(
            indexed.rows,
            vec![
                vec![Cell::Int64(2), Cell::Text("Grace".to_owned())],
                vec![Cell::Int64(3), Cell::Text("Edsger".to_owned())],
            ]
        );
        Ok(())
    }

    #[test]
    fn conjunctive_execution_matches_scan_after_mutation_failure_and_reopen() -> Result<()> {
        let dir = tempdir()?;
        let path = dir.path().join("conjunctive-recovery.log");
        let mut engine = MaintainedIndexEngine::open(&path)?;
        seed(&mut engine)?;
        engine.commit(&[RelOp::UpsertRow {
            table: "users".to_owned(),
            row: vec![
                Cell::Int64(3),
                Cell::Text("theory".to_owned()),
                Cell::Text("Edsger".to_owned()),
            ],
        }])?;
        let conjunctive = conjunctive_query();
        let before_failure = engine.execute_conjunctive(&conjunctive)?;
        let next_tx = engine.relational().next_transaction_id();
        assert!(engine
            .commit(&[RelOp::UpsertRow {
                table: "missing".to_owned(),
                row: vec![Cell::Int64(9)],
            }])
            .is_err());
        assert_eq!(engine.relational().next_transaction_id(), next_tx);
        assert_eq!(engine.execute_conjunctive(&conjunctive)?, before_failure);
        drop(engine);

        let mut reopened = MaintainedIndexEngine::open(&path)?;
        reopened.register_index("users", "team")?;
        assert_eq!(
            reopened.execute_conjunctive(&conjunctive)?,
            query::execute_conjunctive(reopened.relational(), &conjunctive)?
        );
        Ok(())
    }

    #[test]
    fn conjunctive_without_matching_index_falls_back_to_scan() -> Result<()> {
        let dir = tempdir()?;
        let path = dir.path().join("conjunctive-scan.log");
        let mut engine = MaintainedIndexEngine::open(&path)?;
        seed(&mut engine)?;
        let conjunctive = ConjunctiveQuery {
            table: "users".to_owned(),
            predicates: vec![Predicate {
                column: "id".to_owned(),
                op: CompareOp::Gt,
                value: Cell::Int64(1),
            }],
            projection: Projection::All,
        };
        assert_eq!(
            engine.execute_conjunctive(&conjunctive)?,
            query::execute_conjunctive(engine.relational(), &conjunctive)?
        );
        Ok(())
    }

    #[test]
    fn failed_commit_leaves_range_index_and_durable_state_unchanged() -> Result<()> {
        let dir = tempdir()?;
        let path = dir.path().join("failed-range.log");
        let mut engine = MaintainedIndexEngine::open(&path)?;
        seed(&mut engine)?;
        let query = team_query(CompareOp::Ge);
        let before = engine.execute(&query)?;
        let next_tx = engine.relational().next_transaction_id();
        assert!(engine
            .commit(&[RelOp::UpsertRow {
                table: "missing".to_owned(),
                row: vec![Cell::Int64(9)],
            }])
            .is_err());
        assert_eq!(engine.relational().next_transaction_id(), next_tx);
        assert_eq!(engine.execute(&query)?, before);
        Ok(())
    }

    #[test]
    fn reopen_and_reregister_range_index_matches_scan() -> Result<()> {
        let dir = tempdir()?;
        let path = dir.path().join("reopen-range.log");
        let mut engine = MaintainedIndexEngine::open(&path)?;
        seed(&mut engine)?;
        engine.commit(&[RelOp::UpsertRow {
            table: "users".to_owned(),
            row: vec![
                Cell::Int64(5),
                Cell::Text("theory".to_owned()),
                Cell::Text("Barbara".to_owned()),
            ],
        }])?;
        drop(engine);
        let mut reopened = MaintainedIndexEngine::open(&path)?;
        reopened.register_index("users", "team")?;
        let query = team_query(CompareOp::Ge);
        assert_eq!(
            reopened.execute(&query)?,
            query::execute(reopened.relational(), &query)?
        );
        Ok(())
    }
}
