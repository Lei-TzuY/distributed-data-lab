//! Catalog-driven typed query execution over the durable relational engine.
//!
//! Queries are read-only: they evaluate against the replayed relational state without changing the
//! durable v1 transaction format or adding a new persistence boundary. Rows are visited in primary-
//! key order, so a query result is deterministic before and after reopen.

use std::collections::BTreeSet;

use db_core::{DbError, Result};

use crate::relational::{Cell, ColumnType, RelationalEngine, Schema};

/// Typed comparison operator for a single-column predicate.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CompareOp {
    /// Equal to the literal.
    Eq,
    /// Less than the literal.
    Lt,
    /// Less than or equal to the literal.
    Le,
    /// Greater than the literal.
    Gt,
    /// Greater than or equal to the literal.
    Ge,
}

/// One typed predicate resolved through the table catalog.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Predicate {
    /// Column name to compare.
    pub column: String,
    /// Comparison operator.
    pub op: CompareOp,
    /// Typed comparison literal.
    pub value: Cell,
}

/// Projection requested by a query.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Projection {
    /// Return every table column in schema order.
    All,
    /// Return the named columns in the requested order.
    Columns(Vec<String>),
}

/// Read-only catalog-driven query.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Query {
    /// Table to scan.
    pub table: String,
    /// Optional single-column predicate.
    pub predicate: Option<Predicate>,
    /// Output projection.
    pub projection: Projection,
}

/// Read-only query whose predicates are combined by logical AND.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ConjunctiveQuery {
    /// Table to scan.
    pub table: String,
    /// One or more typed predicates, all of which must match.
    pub predicates: Vec<Predicate>,
    /// Output projection.
    pub projection: Projection,
}

/// Materialized deterministic query result.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct QueryResult {
    /// Output column names in row order.
    pub columns: Vec<String>,
    /// Matching rows in primary-key order.
    pub rows: Vec<Vec<Cell>>,
}

type ResolvedPredicate<'a> = (usize, &'a Predicate);
type PreparedConjunction<'a> = (Vec<usize>, Vec<ResolvedPredicate<'a>>);

/// Executes a validated table scan over the durable relational state.
///
/// Column names and literal types are resolved from the catalog before the first row is evaluated.
/// Unknown columns, duplicate projected columns, empty explicit projections, and type mismatches fail
/// without modifying the database.
pub fn execute(engine: &RelationalEngine, query: &Query) -> Result<QueryResult> {
    let schema = engine.schema(&query.table)?;
    let projection = resolve_projection(schema, &query.projection)?;
    let predicate = query
        .predicate
        .as_ref()
        .map(|predicate| resolve_predicate(schema, predicate))
        .transpose()?;

    let columns = projection
        .iter()
        .map(|index| schema.columns[*index].name.clone())
        .collect();
    let mut rows = Vec::new();
    for (_, row) in engine.rows(&query.table)? {
        if let Some((index, predicate)) = predicate {
            if !matches_predicate(&row[index], predicate) {
                continue;
            }
        }
        rows.push(project_row(row, &projection));
    }
    Ok(QueryResult { columns, rows })
}

/// Executes a validated conjunctive table scan used as the correctness oracle for index-assisted
/// execution.
pub fn execute_conjunctive(
    engine: &RelationalEngine,
    query: &ConjunctiveQuery,
) -> Result<QueryResult> {
    let schema = engine.schema(&query.table)?;
    let (projection, predicates) =
        prepare_conjunctive(schema, &query.predicates, &query.projection)?;
    let columns = projection
        .iter()
        .map(|index| schema.columns[*index].name.clone())
        .collect();
    let mut rows = Vec::new();
    for (_, row) in engine.rows(&query.table)? {
        if predicates
            .iter()
            .all(|(index, predicate)| matches_predicate(&row[*index], predicate))
        {
            rows.push(project_row(row, &projection));
        }
    }
    Ok(QueryResult { columns, rows })
}

pub(crate) fn prepare_conjunctive<'a>(
    schema: &Schema,
    predicates: &'a [Predicate],
    projection: &Projection,
) -> Result<PreparedConjunction<'a>> {
    if predicates.is_empty() {
        return Err(DbError::InvalidInput(
            "conjunctive query must contain at least one predicate".to_owned(),
        ));
    }
    let projection = resolve_projection(schema, projection)?;
    let predicates = predicates
        .iter()
        .map(|predicate| resolve_predicate(schema, predicate))
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

fn resolve_predicate<'a>(
    schema: &Schema,
    predicate: &'a Predicate,
) -> Result<ResolvedPredicate<'a>> {
    let index = column_index(schema, &predicate.column)?;
    validate_literal_type(&predicate.value, &schema.columns[index].ty)?;
    Ok((index, predicate))
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

pub(crate) fn matches_predicate(value: &Cell, predicate: &Predicate) -> bool {
    match predicate.op {
        CompareOp::Eq => value == &predicate.value,
        CompareOp::Lt => value < &predicate.value,
        CompareOp::Le => value <= &predicate.value,
        CompareOp::Gt => value > &predicate.value,
        CompareOp::Ge => value >= &predicate.value,
    }
}

pub(crate) fn project_row(row: &[Cell], projection: &[usize]) -> Vec<Cell> {
    projection.iter().map(|index| row[*index].clone()).collect()
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeMap;

    use super::*;
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
                row: vec![Cell::Int64(3), Cell::Text("Edsger".to_owned())],
            },
            RelOp::UpsertRow {
                table: "users".to_owned(),
                row: vec![Cell::Int64(1), Cell::Text("Ada".to_owned())],
            },
            RelOp::UpsertRow {
                table: "users".to_owned(),
                row: vec![Cell::Int64(2), Cell::Text("Grace".to_owned())],
            },
        ])?;
        Ok(())
    }

    #[test]
    fn typed_predicate_projection_is_deterministic_after_reopen() -> Result<()> {
        let dir = tempdir()?;
        let path = dir.path().join("query.log");
        let mut engine = RelationalEngine::open(&path)?;
        seed(&mut engine)?;
        drop(engine);

        let engine = RelationalEngine::open(&path)?;
        let result = execute(
            &engine,
            &Query {
                table: "users".to_owned(),
                predicate: Some(Predicate {
                    column: "id".to_owned(),
                    op: CompareOp::Ge,
                    value: Cell::Int64(2),
                }),
                projection: Projection::Columns(vec!["name".to_owned()]),
            },
        )?;
        assert_eq!(result.columns, vec!["name"]);
        assert_eq!(
            result.rows,
            vec![
                vec![Cell::Text("Grace".to_owned())],
                vec![Cell::Text("Edsger".to_owned())],
            ]
        );
        Ok(())
    }

    #[test]
    fn conjunctive_scan_filters_all_predicates_after_reopen() -> Result<()> {
        let dir = tempdir()?;
        let path = dir.path().join("conjunctive.log");
        let mut engine = RelationalEngine::open(&path)?;
        seed(&mut engine)?;
        drop(engine);
        let engine = RelationalEngine::open(&path)?;
        let result = execute_conjunctive(
            &engine,
            &ConjunctiveQuery {
                table: "users".to_owned(),
                predicates: vec![
                    Predicate {
                        column: "id".to_owned(),
                        op: CompareOp::Ge,
                        value: Cell::Int64(2),
                    },
                    Predicate {
                        column: "name".to_owned(),
                        op: CompareOp::Lt,
                        value: Cell::Text("Grace".to_owned()),
                    },
                ],
                projection: Projection::Columns(vec!["id".to_owned(), "name".to_owned()]),
            },
        )?;
        assert_eq!(
            result.rows,
            vec![vec![Cell::Int64(3), Cell::Text("Edsger".to_owned())]]
        );
        Ok(())
    }

    #[test]
    fn conjunctive_validation_rejects_empty_unknown_and_wrong_type_predicates() -> Result<()> {
        let dir = tempdir()?;
        let path = dir.path().join("conjunctive-validation.log");
        let mut engine = RelationalEngine::open(&path)?;
        seed(&mut engine)?;
        let empty = execute_conjunctive(
            &engine,
            &ConjunctiveQuery {
                table: "users".to_owned(),
                predicates: vec![],
                projection: Projection::All,
            },
        )
        .expect_err("empty conjunction must fail");
        assert!(matches!(empty, DbError::InvalidInput(_)));
        let unknown = execute_conjunctive(
            &engine,
            &ConjunctiveQuery {
                table: "users".to_owned(),
                predicates: vec![Predicate {
                    column: "missing".to_owned(),
                    op: CompareOp::Eq,
                    value: Cell::Int64(1),
                }],
                projection: Projection::All,
            },
        )
        .expect_err("unknown predicate column must fail");
        assert!(matches!(unknown, DbError::InvalidInput(_)));
        let wrong_type = execute_conjunctive(
            &engine,
            &ConjunctiveQuery {
                table: "users".to_owned(),
                predicates: vec![Predicate {
                    column: "id".to_owned(),
                    op: CompareOp::Eq,
                    value: Cell::Text("1".to_owned()),
                }],
                projection: Projection::All,
            },
        )
        .expect_err("wrong predicate type must fail");
        assert!(matches!(wrong_type, DbError::InvalidInput(_)));
        Ok(())
    }

    #[test]
    fn query_validation_rejects_unknown_duplicate_and_wrong_type_columns() -> Result<()> {
        let dir = tempdir()?;
        let path = dir.path().join("validation.log");
        let mut engine = RelationalEngine::open(&path)?;
        seed(&mut engine)?;

        let unknown = execute(
            &engine,
            &Query {
                table: "users".to_owned(),
                predicate: None,
                projection: Projection::Columns(vec!["missing".to_owned()]),
            },
        )
        .expect_err("unknown projection must fail");
        assert!(matches!(unknown, DbError::InvalidInput(_)));

        let duplicate = execute(
            &engine,
            &Query {
                table: "users".to_owned(),
                predicate: None,
                projection: Projection::Columns(vec!["id".to_owned(), "id".to_owned()]),
            },
        )
        .expect_err("duplicate projection must fail");
        assert!(matches!(duplicate, DbError::InvalidInput(_)));

        let wrong_type = execute(
            &engine,
            &Query {
                table: "users".to_owned(),
                predicate: Some(Predicate {
                    column: "id".to_owned(),
                    op: CompareOp::Eq,
                    value: Cell::Text("1".to_owned()),
                }),
                projection: Projection::All,
            },
        )
        .expect_err("typed predicate mismatch must fail");
        assert!(matches!(wrong_type, DbError::InvalidInput(_)));
        Ok(())
    }

    #[test]
    fn predicate_scan_matches_reference_oracle_across_mutation_and_reopen() -> Result<()> {
        let dir = tempdir()?;
        let path = dir.path().join("oracle.log");
        let mut engine = RelationalEngine::open(&path)?;
        engine.commit(&[RelOp::CreateTable {
            name: "users".to_owned(),
            schema: users_schema(),
        }])?;

        let mut oracle = BTreeMap::new();
        for (id, name) in [(4, "Barbara"), (1, "Ada"), (3, "Edsger"), (2, "Grace")] {
            engine.commit(&[RelOp::UpsertRow {
                table: "users".to_owned(),
                row: vec![Cell::Int64(id), Cell::Text(name.to_owned())],
            }])?;
            oracle.insert(id, name.to_owned());
        }
        engine.commit(&[RelOp::DeleteRow {
            table: "users".to_owned(),
            key: Cell::Int64(3),
        }])?;
        oracle.remove(&3);
        drop(engine);

        let engine = RelationalEngine::open(&path)?;
        let observed = execute(
            &engine,
            &Query {
                table: "users".to_owned(),
                predicate: Some(Predicate {
                    column: "id".to_owned(),
                    op: CompareOp::Gt,
                    value: Cell::Int64(1),
                }),
                projection: Projection::All,
            },
        )?
        .rows
        .into_iter()
        .map(|row| match (&row[0], &row[1]) {
            (Cell::Int64(id), Cell::Text(name)) => (*id, name.clone()),
            _ => unreachable!("validated users schema"),
        })
        .collect::<BTreeMap<_, _>>();
        let expected = oracle
            .into_iter()
            .filter(|(id, _)| *id > 1)
            .collect::<BTreeMap<_, _>>();
        assert_eq!(observed, expected);
        Ok(())
    }
}
