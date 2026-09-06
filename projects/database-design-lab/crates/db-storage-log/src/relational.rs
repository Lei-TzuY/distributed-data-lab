//! Reusable durable relational layer on top of the append-log engine.
//!
//! Each relational transaction is encoded as one versioned, checksummed value and committed with a
//! single [`LogEngine::put`]. The append-log durability boundary therefore remains unchanged: schema
//! and row mutations in one transaction replay all-or-none after reopen.

use std::collections::BTreeMap;
use std::path::Path;

use db_core::{DbError, KvEngine, Result, MAX_VALUE_BYTES};

use crate::LogEngine;

const TX_KEY_PREFIX: &[u8] = b"\0db-lab-rel-v1/";
const MAGIC: [u8; 8] = *b"DBRELTX1";
const VERSION: u16 = 1;
const HEADER_LEN: usize = 24;
const TRAILER_LEN: usize = 4;
const MAX_OPS: usize = 1024;
const MAX_COLUMNS: usize = 64;
const MAX_NAME_BYTES: usize = 255;

const OP_CREATE_TABLE: u8 = 1;
const OP_UPSERT_ROW: u8 = 2;
const OP_DELETE_ROW: u8 = 3;
const TYPE_I64: u8 = 1;
const TYPE_TEXT: u8 = 2;

/// Scalar column type supported by the durable relational v1 format.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ColumnType {
    /// Signed 64-bit integer.
    Int64,
    /// UTF-8 text.
    Text,
}

/// One named table column.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Column {
    /// Column name.
    pub name: String,
    /// Column type.
    pub ty: ColumnType,
}

/// Fixed table schema with one primary-key column.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Schema {
    /// Columns in row order.
    pub columns: Vec<Column>,
    /// Index of the primary-key column.
    pub primary_key: usize,
}

/// Typed relational cell.
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord)]
pub enum Cell {
    /// Signed 64-bit integer value.
    Int64(i64),
    /// UTF-8 text value.
    Text(String),
}

/// Atomic relational mutation operation.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum RelOp {
    /// Create a new table.
    CreateTable { name: String, schema: Schema },
    /// Insert or replace one row by its primary key.
    UpsertRow { table: String, row: Vec<Cell> },
    /// Delete one row by primary key. Missing rows are a no-op.
    DeleteRow { table: String, key: Cell },
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct TableState {
    schema: Schema,
    rows: BTreeMap<Cell, Vec<Cell>>,
}

/// Single-process durable relational engine backed by [`LogEngine`].
#[derive(Debug)]
pub struct RelationalEngine {
    backing: LogEngine,
    tables: BTreeMap<String, TableState>,
    next_tx_id: u64,
}

impl RelationalEngine {
    /// Opens or creates a relational database and deterministically replays durable transactions.
    pub fn open(path: impl AsRef<Path>) -> Result<Self> {
        let path = path.as_ref();
        let backing = LogEngine::open(path)?;
        let inspection = LogEngine::inspect(path, true)?;
        let mut tables = BTreeMap::new();
        let mut expected_tx_id = 1_u64;

        for entry in inspection.entries {
            let key = entry.key.into_vec();
            let Some(tx_id) = parse_tx_key(&key)? else {
                return Err(corruption(format!(
                    "relational database contains unexpected live key {}",
                    hex(&key)
                )));
            };
            if tx_id != expected_tx_id {
                return Err(corruption(format!(
                    "relational transaction id discontinuity: expected {expected_tx_id}, found {tx_id}"
                )));
            }
            let bytes = entry
                .value
                .ok_or_else(|| corruption("inspection omitted relational transaction value"))?
                .into_vec();
            let ops = decode_transaction(&bytes, tx_id)?;
            apply_ops(&mut tables, &ops)?;
            expected_tx_id = expected_tx_id
                .checked_add(1)
                .ok_or_else(|| corruption("relational transaction id overflow during replay"))?;
        }

        Ok(Self {
            backing,
            tables,
            next_tx_id: expected_tx_id,
        })
    }

    /// Atomically validates and commits all operations as one durable transaction.
    pub fn commit(&mut self, ops: &[RelOp]) -> Result<u64> {
        if ops.is_empty() {
            return Err(DbError::InvalidInput(
                "relational transaction must contain at least one operation".to_owned(),
            ));
        }
        if ops.len() > MAX_OPS {
            return Err(DbError::InvalidInput(format!(
                "relational transaction has {} operations; maximum is {MAX_OPS}",
                ops.len()
            )));
        }

        let mut candidate = self.tables.clone();
        apply_ops(&mut candidate, ops)?;
        let tx_id = self.next_tx_id;
        let encoded = encode_transaction(tx_id, ops)?;
        self.backing.put(&tx_key(tx_id), &encoded)?;
        self.tables = candidate;
        self.next_tx_id = tx_id.checked_add(1).ok_or_else(|| {
            DbError::InvalidInput("relational transaction id space exhausted".to_owned())
        })?;
        Ok(tx_id)
    }

    /// Returns the transaction id that the next successful commit will receive.
    #[must_use]
    pub fn next_transaction_id(&self) -> u64 {
        self.next_tx_id
    }

    /// Iterates catalog entries in table-name order.
    pub fn catalog(&self) -> impl Iterator<Item = (&str, &Schema)> {
        self.tables
            .iter()
            .map(|(name, state)| (name.as_str(), &state.schema))
    }

    /// Returns a table schema.
    pub fn schema(&self, table: &str) -> Result<&Schema> {
        self.tables
            .get(table)
            .map(|state| &state.schema)
            .ok_or_else(|| DbError::InvalidInput(format!("unknown table {table}")))
    }

    /// Returns one row by primary key.
    pub fn row(&self, table: &str, key: &Cell) -> Result<Option<&[Cell]>> {
        let table = self
            .tables
            .get(table)
            .ok_or_else(|| DbError::InvalidInput(format!("unknown table {table}")))?;
        validate_cell_type(key, &table.schema.columns[table.schema.primary_key].ty)?;
        Ok(table.rows.get(key).map(Vec::as_slice))
    }

    /// Iterates rows in primary-key order.
    pub fn rows(&self, table: &str) -> Result<impl Iterator<Item = (&Cell, &[Cell])>> {
        let table = self
            .tables
            .get(table)
            .ok_or_else(|| DbError::InvalidInput(format!("unknown table {table}")))?;
        Ok(table.rows.iter().map(|(key, row)| (key, row.as_slice())))
    }
}

fn apply_ops(tables: &mut BTreeMap<String, TableState>, ops: &[RelOp]) -> Result<()> {
    for op in ops {
        match op {
            RelOp::CreateTable { name, schema } => {
                validate_name(name, "table")?;
                validate_schema(schema)?;
                if tables.contains_key(name) {
                    return Err(DbError::InvalidInput(format!(
                        "table {name} already exists"
                    )));
                }
                tables.insert(
                    name.clone(),
                    TableState {
                        schema: schema.clone(),
                        rows: BTreeMap::new(),
                    },
                );
            }
            RelOp::UpsertRow { table, row } => {
                let state = tables
                    .get_mut(table)
                    .ok_or_else(|| DbError::InvalidInput(format!("unknown table {table}")))?;
                validate_row(&state.schema, row)?;
                let key = row[state.schema.primary_key].clone();
                state.rows.insert(key, row.clone());
            }
            RelOp::DeleteRow { table, key } => {
                let state = tables
                    .get_mut(table)
                    .ok_or_else(|| DbError::InvalidInput(format!("unknown table {table}")))?;
                validate_cell_type(key, &state.schema.columns[state.schema.primary_key].ty)?;
                state.rows.remove(key);
            }
        }
    }
    Ok(())
}

fn validate_schema(schema: &Schema) -> Result<()> {
    if schema.columns.is_empty() || schema.columns.len() > MAX_COLUMNS {
        return Err(DbError::InvalidInput(format!(
            "schema column count must be in 1..={MAX_COLUMNS}"
        )));
    }
    if schema.primary_key >= schema.columns.len() {
        return Err(DbError::InvalidInput(
            "schema primary key index is outside the column list".to_owned(),
        ));
    }
    let mut seen = BTreeMap::<&str, ()>::new();
    for column in &schema.columns {
        validate_name(&column.name, "column")?;
        if seen.insert(&column.name, ()).is_some() {
            return Err(DbError::InvalidInput(format!(
                "duplicate column {}",
                column.name
            )));
        }
    }
    Ok(())
}

fn validate_row(schema: &Schema, row: &[Cell]) -> Result<()> {
    if row.len() != schema.columns.len() {
        return Err(DbError::InvalidInput(format!(
            "row has {} cells but schema requires {}",
            row.len(),
            schema.columns.len()
        )));
    }
    for (cell, column) in row.iter().zip(&schema.columns) {
        validate_cell_type(cell, &column.ty)?;
    }
    Ok(())
}

fn validate_cell_type(cell: &Cell, ty: &ColumnType) -> Result<()> {
    match (cell, ty) {
        (Cell::Int64(_), ColumnType::Int64) => Ok(()),
        (Cell::Text(value), ColumnType::Text) => {
            if value.len() > MAX_VALUE_BYTES {
                return Err(DbError::InvalidInput(
                    "text cell exceeds backing value limit".to_owned(),
                ));
            }
            Ok(())
        }
        _ => Err(DbError::InvalidInput("row cell type mismatch".to_owned())),
    }
}

fn validate_name(name: &str, kind: &str) -> Result<()> {
    if name.is_empty() || name.len() > MAX_NAME_BYTES {
        return Err(DbError::InvalidInput(format!(
            "{kind} name length must be in 1..={MAX_NAME_BYTES} bytes"
        )));
    }
    if !name
        .bytes()
        .all(|byte| byte.is_ascii_alphanumeric() || byte == b'_')
    {
        return Err(DbError::InvalidInput(format!(
            "{kind} name must contain only ASCII letters, digits, or underscore"
        )));
    }
    Ok(())
}

fn tx_key(tx_id: u64) -> Vec<u8> {
    let mut key = Vec::with_capacity(TX_KEY_PREFIX.len() + 8);
    key.extend_from_slice(TX_KEY_PREFIX);
    key.extend_from_slice(&tx_id.to_be_bytes());
    key
}

fn parse_tx_key(key: &[u8]) -> Result<Option<u64>> {
    let Some(suffix) = key.strip_prefix(TX_KEY_PREFIX) else {
        return Ok(None);
    };
    if suffix.len() != 8 {
        return Err(corruption(
            "relational transaction key has invalid id width",
        ));
    }
    let mut bytes = [0_u8; 8];
    bytes.copy_from_slice(suffix);
    let tx_id = u64::from_be_bytes(bytes);
    if tx_id == 0 {
        return Err(corruption("relational transaction id zero is reserved"));
    }
    Ok(Some(tx_id))
}

fn encode_transaction(tx_id: u64, ops: &[RelOp]) -> Result<Vec<u8>> {
    let count = u32::try_from(ops.len()).map_err(|_| {
        DbError::InvalidInput("relational operation count does not fit u32".to_owned())
    })?;
    let mut out = Vec::new();
    out.extend_from_slice(&MAGIC);
    out.extend_from_slice(&VERSION.to_le_bytes());
    out.extend_from_slice(&0_u16.to_le_bytes());
    out.extend_from_slice(&tx_id.to_le_bytes());
    out.extend_from_slice(&count.to_le_bytes());

    for op in ops {
        match op {
            RelOp::CreateTable { name, schema } => {
                out.push(OP_CREATE_TABLE);
                put_string(&mut out, name)?;
                put_u16(&mut out, schema.primary_key)?;
                put_u16(&mut out, schema.columns.len())?;
                for column in &schema.columns {
                    put_string(&mut out, &column.name)?;
                    out.push(match column.ty {
                        ColumnType::Int64 => TYPE_I64,
                        ColumnType::Text => TYPE_TEXT,
                    });
                }
            }
            RelOp::UpsertRow { table, row } => {
                out.push(OP_UPSERT_ROW);
                put_string(&mut out, table)?;
                put_u16(&mut out, row.len())?;
                for cell in row {
                    encode_cell(&mut out, cell)?;
                }
            }
            RelOp::DeleteRow { table, key } => {
                out.push(OP_DELETE_ROW);
                put_string(&mut out, table)?;
                encode_cell(&mut out, key)?;
            }
        }
        if out.len() + TRAILER_LEN > MAX_VALUE_BYTES {
            return Err(DbError::InvalidInput(format!(
                "encoded relational transaction exceeds the {MAX_VALUE_BYTES}-byte backing value limit"
            )));
        }
    }
    let checksum = crc32fast::hash(&out);
    out.extend_from_slice(&checksum.to_le_bytes());
    Ok(out)
}

fn decode_transaction(bytes: &[u8], expected_tx_id: u64) -> Result<Vec<RelOp>> {
    if bytes.len() < HEADER_LEN + TRAILER_LEN {
        return Err(corruption("relational transaction is truncated"));
    }
    if bytes[..8] != MAGIC {
        return Err(corruption("relational transaction magic mismatch"));
    }
    let version = read_u16(bytes, 8)?;
    if version != VERSION {
        return Err(DbError::UnsupportedVersion {
            format: "relational transaction",
            found: u64::from(version),
            supported: u64::from(VERSION),
        });
    }
    if read_u16(bytes, 10)? != 0 {
        return Err(corruption(
            "relational transaction reserved header bits are nonzero",
        ));
    }
    let tx_id = read_u64(bytes, 12)?;
    if tx_id != expected_tx_id {
        return Err(corruption(format!(
            "relational transaction key/value id mismatch: key={expected_tx_id}, value={tx_id}"
        )));
    }
    let count = usize::try_from(read_u32(bytes, 20)?)
        .map_err(|_| corruption("relational operation count does not fit usize"))?;
    if count == 0 || count > MAX_OPS {
        return Err(corruption(format!(
            "relational operation count {count} is outside 1..={MAX_OPS}"
        )));
    }
    let trailer = bytes.len() - TRAILER_LEN;
    let expected_crc = read_u32(bytes, trailer)?;
    let actual_crc = crc32fast::hash(&bytes[..trailer]);
    if expected_crc != actual_crc {
        return Err(corruption("relational transaction checksum mismatch"));
    }

    let mut cursor = HEADER_LEN;
    let mut ops = Vec::with_capacity(count);
    for _ in 0..count {
        let kind = take_byte(bytes, &mut cursor, trailer)?;
        let op = match kind {
            OP_CREATE_TABLE => {
                let name = take_string(bytes, &mut cursor, trailer)?;
                let primary_key = usize::from(take_u16(bytes, &mut cursor, trailer)?);
                let column_count = usize::from(take_u16(bytes, &mut cursor, trailer)?);
                if column_count == 0 || column_count > MAX_COLUMNS {
                    return Err(corruption("encoded schema column count is invalid"));
                }
                let mut columns = Vec::with_capacity(column_count);
                for _ in 0..column_count {
                    let name = take_string(bytes, &mut cursor, trailer)?;
                    let ty = match take_byte(bytes, &mut cursor, trailer)? {
                        TYPE_I64 => ColumnType::Int64,
                        TYPE_TEXT => ColumnType::Text,
                        _ => return Err(corruption("encoded schema has unknown column type")),
                    };
                    columns.push(Column { name, ty });
                }
                RelOp::CreateTable {
                    name,
                    schema: Schema {
                        columns,
                        primary_key,
                    },
                }
            }
            OP_UPSERT_ROW => {
                let table = take_string(bytes, &mut cursor, trailer)?;
                let cell_count = usize::from(take_u16(bytes, &mut cursor, trailer)?);
                if cell_count == 0 || cell_count > MAX_COLUMNS {
                    return Err(corruption("encoded row cell count is invalid"));
                }
                let mut row = Vec::with_capacity(cell_count);
                for _ in 0..cell_count {
                    row.push(decode_cell(bytes, &mut cursor, trailer)?);
                }
                RelOp::UpsertRow { table, row }
            }
            OP_DELETE_ROW => RelOp::DeleteRow {
                table: take_string(bytes, &mut cursor, trailer)?,
                key: decode_cell(bytes, &mut cursor, trailer)?,
            },
            _ => {
                return Err(corruption(
                    "relational transaction contains unknown operation",
                ));
            }
        };
        ops.push(op);
    }
    if cursor != trailer {
        return Err(corruption(
            "relational transaction has trailing payload bytes",
        ));
    }
    Ok(ops)
}

fn encode_cell(out: &mut Vec<u8>, cell: &Cell) -> Result<()> {
    match cell {
        Cell::Int64(value) => {
            out.push(TYPE_I64);
            out.extend_from_slice(&value.to_le_bytes());
        }
        Cell::Text(value) => {
            out.push(TYPE_TEXT);
            put_string(out, value)?;
        }
    }
    Ok(())
}

fn decode_cell(bytes: &[u8], cursor: &mut usize, limit: usize) -> Result<Cell> {
    match take_byte(bytes, cursor, limit)? {
        TYPE_I64 => {
            let slice = take(bytes, cursor, 8, limit)?;
            let mut value = [0_u8; 8];
            value.copy_from_slice(slice);
            Ok(Cell::Int64(i64::from_le_bytes(value)))
        }
        TYPE_TEXT => Ok(Cell::Text(take_string(bytes, cursor, limit)?)),
        _ => Err(corruption("encoded row has unknown cell type")),
    }
}

fn put_string(out: &mut Vec<u8>, value: &str) -> Result<()> {
    let len = u16::try_from(value.len())
        .map_err(|_| DbError::InvalidInput("string length does not fit u16".to_owned()))?;
    out.extend_from_slice(&len.to_le_bytes());
    out.extend_from_slice(value.as_bytes());
    Ok(())
}

fn put_u16(out: &mut Vec<u8>, value: usize) -> Result<()> {
    let value = u16::try_from(value)
        .map_err(|_| DbError::InvalidInput("relational field does not fit u16".to_owned()))?;
    out.extend_from_slice(&value.to_le_bytes());
    Ok(())
}

fn take_string(bytes: &[u8], cursor: &mut usize, limit: usize) -> Result<String> {
    let len = usize::from(take_u16(bytes, cursor, limit)?);
    let value = take(bytes, cursor, len, limit)?;
    String::from_utf8(value.to_vec()).map_err(|_| corruption("encoded string is not UTF-8"))
}

fn take_u16(bytes: &[u8], cursor: &mut usize, limit: usize) -> Result<u16> {
    let slice = take(bytes, cursor, 2, limit)?;
    Ok(u16::from_le_bytes([slice[0], slice[1]]))
}

fn take_byte(bytes: &[u8], cursor: &mut usize, limit: usize) -> Result<u8> {
    Ok(take(bytes, cursor, 1, limit)?[0])
}

fn take<'a>(bytes: &'a [u8], cursor: &mut usize, len: usize, limit: usize) -> Result<&'a [u8]> {
    let end = cursor
        .checked_add(len)
        .ok_or_else(|| corruption("relational payload offset overflow"))?;
    if end > limit {
        return Err(corruption("relational payload is truncated"));
    }
    let value = &bytes[*cursor..end];
    *cursor = end;
    Ok(value)
}

fn read_u16(bytes: &[u8], offset: usize) -> Result<u16> {
    let slice = bytes
        .get(offset..offset + 2)
        .ok_or_else(|| corruption("relational u16 field is truncated"))?;
    Ok(u16::from_le_bytes([slice[0], slice[1]]))
}

fn read_u32(bytes: &[u8], offset: usize) -> Result<u32> {
    let slice = bytes
        .get(offset..offset + 4)
        .ok_or_else(|| corruption("relational u32 field is truncated"))?;
    Ok(u32::from_le_bytes([slice[0], slice[1], slice[2], slice[3]]))
}

fn read_u64(bytes: &[u8], offset: usize) -> Result<u64> {
    let slice = bytes
        .get(offset..offset + 8)
        .ok_or_else(|| corruption("relational u64 field is truncated"))?;
    let mut value = [0_u8; 8];
    value.copy_from_slice(slice);
    Ok(u64::from_le_bytes(value))
}

fn corruption(reason: impl Into<String>) -> DbError {
    DbError::Corruption {
        offset: 0,
        reason: reason.into(),
    }
}

fn hex(bytes: &[u8]) -> String {
    let mut out = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        use std::fmt::Write as _;
        let _ = write!(out, "{byte:02x}");
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
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

    #[test]
    fn schema_rows_and_catalog_survive_reopen() -> Result<()> {
        let dir = tempdir()?;
        let path = dir.path().join("relational.log");
        let mut engine = RelationalEngine::open(&path)?;
        assert_eq!(
            engine.commit(&[
                RelOp::CreateTable {
                    name: "users".to_owned(),
                    schema: users_schema(),
                },
                RelOp::UpsertRow {
                    table: "users".to_owned(),
                    row: vec![Cell::Int64(1), Cell::Text("Ada".to_owned())],
                },
                RelOp::UpsertRow {
                    table: "users".to_owned(),
                    row: vec![Cell::Int64(2), Cell::Text("Grace".to_owned())],
                },
            ])?,
            1
        );
        drop(engine);

        let engine = RelationalEngine::open(&path)?;
        assert_eq!(
            engine.catalog().collect::<Vec<_>>(),
            vec![("users", &users_schema())]
        );
        assert_eq!(engine.schema("users")?, &users_schema());
        assert_eq!(
            engine.row("users", &Cell::Int64(1))?,
            Some(&[Cell::Int64(1), Cell::Text("Ada".to_owned())][..])
        );
        assert_eq!(engine.next_transaction_id(), 2);
        Ok(())
    }

    #[test]
    fn invalid_row_rolls_back_entire_transaction_without_consuming_id() -> Result<()> {
        let dir = tempdir()?;
        let path = dir.path().join("rollback.log");
        let mut engine = RelationalEngine::open(&path)?;
        engine.commit(&[RelOp::CreateTable {
            name: "users".to_owned(),
            schema: users_schema(),
        }])?;

        let error = engine
            .commit(&[
                RelOp::UpsertRow {
                    table: "users".to_owned(),
                    row: vec![Cell::Int64(1), Cell::Text("Ada".to_owned())],
                },
                RelOp::UpsertRow {
                    table: "users".to_owned(),
                    row: vec![
                        Cell::Text("wrong-key-type".to_owned()),
                        Cell::Text("bad".to_owned()),
                    ],
                },
            ])
            .expect_err("mixed valid/invalid relational transaction must fail atomically");
        assert!(matches!(error, DbError::InvalidInput(_)));
        assert!(engine.row("users", &Cell::Int64(1))?.is_none());
        assert_eq!(engine.next_transaction_id(), 2);
        assert_eq!(
            engine.commit(&[RelOp::UpsertRow {
                table: "users".to_owned(),
                row: vec![Cell::Int64(1), Cell::Text("Ada".to_owned())],
            }])?,
            2
        );
        Ok(())
    }

    #[test]
    fn row_mutations_match_reference_model_across_reopen() -> Result<()> {
        let dir = tempdir()?;
        let path = dir.path().join("oracle.log");
        let mut engine = RelationalEngine::open(&path)?;
        engine.commit(&[RelOp::CreateTable {
            name: "users".to_owned(),
            schema: users_schema(),
        }])?;

        let mut oracle = BTreeMap::new();
        for (id, name) in [(3, "Edsger"), (1, "Ada"), (2, "Grace"), (1, "Ada Lovelace")] {
            engine.commit(&[RelOp::UpsertRow {
                table: "users".to_owned(),
                row: vec![Cell::Int64(id), Cell::Text(name.to_owned())],
            }])?;
            oracle.insert(id, name.to_owned());
        }
        engine.commit(&[RelOp::DeleteRow {
            table: "users".to_owned(),
            key: Cell::Int64(2),
        }])?;
        oracle.remove(&2);
        drop(engine);

        let engine = RelationalEngine::open(&path)?;
        let observed = engine
            .rows("users")?
            .map(|(key, row)| match (key, &row[1]) {
                (Cell::Int64(id), Cell::Text(name)) => (*id, name.clone()),
                _ => unreachable!("validated users schema"),
            })
            .collect::<BTreeMap<_, _>>();
        assert_eq!(observed, oracle);
        Ok(())
    }

    #[test]
    fn corrupted_relational_record_fails_closed_on_reopen() -> Result<()> {
        let dir = tempdir()?;
        let path = dir.path().join("corrupt.log");
        let mut raw = LogEngine::open(&path)?;
        let mut encoded = encode_transaction(
            1,
            &[RelOp::CreateTable {
                name: "users".to_owned(),
                schema: users_schema(),
            }],
        )?;
        encoded[HEADER_LEN] ^= 0x01;
        raw.put(&tx_key(1), &encoded)?;
        drop(raw);

        let error = RelationalEngine::open(&path)
            .expect_err("corrupted relational payload must fail closed");
        assert!(matches!(error, DbError::Corruption { .. }));
        Ok(())
    }
}
