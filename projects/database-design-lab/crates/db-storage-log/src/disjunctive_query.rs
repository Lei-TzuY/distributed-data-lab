//! Logical-OR query execution over durable relational state and maintained secondary indexes.
//!
//! Disjunctive queries are read-only and do not change the durable transaction format. The scan
//! path is the correctness oracle. The maintained-index path executes each typed predicate through
//! the existing index-aware executor, unions overlapping full-row candidates by primary key, then
//! applies the requested projection in deterministic primary-key order.

use std::collections::{BTreeMap, BTreeSet};

use db_core::{DbError, Result};

use crate::maintained_index::MaintainedIndexEngine;
use crate::query::{CompareOp, Predicate, Projection, Query, QueryResult};
use crate::relational::{Cell, ColumnType, RelationalEngine, Schema};

/// Read-only query whose predicates are combined by logical OR.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DisjunctiveQuery {
    /// Table to query.
    pub table: String,
    /// One or more typed predicates; a row matches when any predicate matches.
    pub predicates: Vec<Predicate>,
    /// Output projection.
    pub projection: Projection,
}

type ResolvedPredicate<'a> = (usize, &'a Predicate);
type PreparedDisjunction<'a> = (Vec<usize>, Vec<ResolvedPredicate<'a>>);

/// Executes a validated full scan and serves as the correctness oracle for index-union execution.
pub fn execute_scan(engine: &RelationalEngine, query: &DisjunctiveQuery) -> Result<QueryResult> {
    let schema = engine.schema(&query.table)?;
    let (projection, predicates) = prepare(schema, &query.predicates, &query.projection)?;
    let columns = projected_columns(schema, &projection);
    let mut rows = Vec::new();
    for (_, row) in engine.rows(&query.table)? {
        if predicates
            .iter()
            .any(|(column, predicate)| matches_predicate(&row[*column], predicate))
        {
            rows.push(project_row(row, &projection));
        }
    }
    Ok(QueryResult { columns, rows })
}

/// Executes a logical OR through transaction-maintained indexes when available.
///
/// Every predicate is validated before execution. Each predicate is then evaluated through the
/// maintained engine's existing index-aware single-predicate path. Full-row candidates are unioned
/// by primary key so overlapping index ranges cannot duplicate output rows. Predicates without a
/// registered index retain the same semantics through that executor's scan fallback.
pub fn execute_indexed(
    engine: &MaintainedIndexEngine,
    query: &DisjunctiveQuery,
) -> Result<QueryResult> {
    let schema = engine.relational().schema(&query.table)?;
    let (projection, predicates) = prepare(schema, &query.predicates, &query.projection)?;
    let columns = projected_columns(schema, &projection);
    let mut candidates = BTreeMap::new();

    for (_, predicate) in predicates {
        let result = engine.execute(&Query {
            table: query.table.clone(),
            predicate: Some(predicate.clone()),
            projection: Projection::All,
        })?;
        for row in result.rows {
            let primary_key = row
                .get(schema.primary_key)
                .ok_or_else(|| DbError::Corruption {
                    offset: 0,
                    reason: "disjunctive candidate row omitted primary key".to_owned(),
                })?
                .clone();
            candidates.insert(primary_key, row);
        }
    }

    let rows = candidates
        .into_values()
        .map(|row| project_row(&row, &projection))
        .collect();
    Ok(QueryResult { columns, rows })
}

fn prepare<'a>(
    schema: &Schema,
    predicates: &'a [Predicate],
    projection: &Projection,
) -> Result<PreparedDisjunction<'a>> {
    if predicates.is_empty() {
        return Err(DbError::InvalidInput(
            "disjunctive query must contain at least one predicate".to_owned(),
        ));
    }
    let projection = resolve_projection(schema, projection)?;
    let predicates = predicates
        .iter()
        .map(|predicate| {
            let column = column_index(schema, &predicate.column)?;
            validate_literal_type(&predicate.value, &schema.columns[column].ty)?;
            Ok((column, predicate))
        })
        .collect::<Result<Vec<_>>>()?;
    Ok((projection, predicates))
}

fn resolve_projection(schema: &Schema, projection: &Projection) -> Result<Vec<usize>> {
    match projection {
        Projection::All => Ok((0..schema.columns.len()).collect()),
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
                    column_index(schema, name)
                })
                .collect()
        }
    }
}

fn column_index(schema: &Schema, name: &str) -> Result<usize> {
    schema
        .columns
        .iter()
        .position(|column| column.name == name)
        .ok_or_else(|| DbError::InvalidInput(format!("unknown column {name}")))
}

fn validate_literal_type(value: &Cell, ty: &ColumnType) -> Result<()> {
    match (value, ty) {
        (Cell::Int64(_), ColumnType::Int64) | (Cell::Text(_), ColumnType::Text) => Ok(()),
        _ => Err(DbError::InvalidInput(
            "query predicate literal type does not match column type".to_owned(),
        )),
    }
}

fn matches_predicate(value: &Cell, predicate: &Predicate) -> bool {
    match predicate.op {
        CompareOp::Eq => value == &predicate.value,
        CompareOp::Lt => value < &predicate.value,
        CompareOp::Le => value <= &predicate.value,
        CompareOp::Gt => value > &predicate.value,
        CompareOp::Ge => value >= &predicate.value,
    }
}

fn projected_columns(schema: &Schema, projection: &[usize]) -> Vec<String> {
    projection
        .iter()
        .map(|index| schema.columns[*index].name.clone())
        .collect()
}

fn project_row(row: &[Cell], projection: &[usize]) -> Vec<Cell> {
    projection.iter().map(|index| row[*index].clone()).collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::query::Predicate;
    use crate::relational::{Column, RelOp};
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
        engine.register_index("users", "id")?;
        Ok(())
    }

    fn disjunction() -> DisjunctiveQuery {
        DisjunctiveQuery {
            table: "users".to_owned(),
            predicates: vec![
                Predicate {
                    column: "team".to_owned(),
                    op: CompareOp::Eq,
                    value: Cell::Text("systems".to_owned()),
                },
                Predicate {
                    column: "id".to_owned(),
                    op: CompareOp::Ge,
                    value: Cell::Int64(3),
                },
            ],
            projection: Projection::Columns(vec!["id".to_owned(), "name".to_owned()]),
        }
    }

    #[test]
    fn overlapping_index_candidates_union_and_deduplicate_against_scan_oracle() -> Result<()> {
        let dir = tempdir()?;
        let path = dir.path().join("disjunction.log");
        let mut engine = MaintainedIndexEngine::open(&path)?;
        seed(&mut engine)?;
        let query = disjunction();
        let indexed = execute_indexed(&engine, &query)?;
        let scanned = execute_scan(engine.relational(), &query)?;
        assert_eq!(indexed, scanned);
        assert_eq!(
            indexed.rows,
            vec![
                vec![Cell::Int64(2), Cell::Text("Grace".to_owned())],
                vec![Cell::Int64(3), Cell::Text("Edsger".to_owned())],
                vec![Cell::Int64(4), Cell::Text("Donald".to_owned())],
            ]
        );
        Ok(())
    }

    #[test]
    fn mutation_and_reopen_preserve_disjunctive_index_union_equivalence() -> Result<()> {
        let dir = tempdir()?;
        let path = dir.path().join("disjunction-reopen.log");
        let mut engine = MaintainedIndexEngine::open(&path)?;
        seed(&mut engine)?;
        engine.commit(&[
            RelOp::DeleteRow {
                table: "users".to_owned(),
                key: Cell::Int64(1),
            },
            RelOp::UpsertRow {
                table: "users".to_owned(),
                row: vec![
                    Cell::Int64(5),
                    Cell::Text("systems".to_owned()),
                    Cell::Text("Barbara".to_owned()),
                ],
            },
        ])?;
        let query = disjunction();
        assert_eq!(
            execute_indexed(&engine, &query)?,
            execute_scan(engine.relational(), &query)?
        );
        drop(engine);

        let mut reopened = MaintainedIndexEngine::open(&path)?;
        reopened.register_index("users", "team")?;
        reopened.register_index("users", "id")?;
        assert_eq!(
            execute_indexed(&reopened, &query)?,
            execute_scan(reopened.relational(), &query)?
        );
        Ok(())
    }

    #[test]
    fn validation_rejects_empty_and_wrong_type_disjunctions_before_execution() -> Result<()> {
        let dir = tempdir()?;
        let path = dir.path().join("disjunction-validation.log");
        let mut engine = MaintainedIndexEngine::open(&path)?;
        seed(&mut engine)?;
        let empty = DisjunctiveQuery {
            table: "users".to_owned(),
            predicates: vec![],
            projection: Projection::All,
        };
        assert!(matches!(
            execute_indexed(&engine, &empty),
            Err(DbError::InvalidInput(_))
        ));

        let wrong_type = DisjunctiveQuery {
            table: "users".to_owned(),
            predicates: vec![Predicate {
                column: "id".to_owned(),
                op: CompareOp::Eq,
                value: Cell::Text("three".to_owned()),
            }],
            projection: Projection::All,
        };
        assert!(matches!(
            execute_indexed(&engine, &wrong_type),
            Err(DbError::InvalidInput(_))
        ));
        assert!(matches!(
            execute_scan(engine.relational(), &wrong_type),
            Err(DbError::InvalidInput(_))
        ));
        Ok(())
    }
}
