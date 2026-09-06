use std::io;

use serde::Serialize;
use thiserror::Error;

/// Result type shared by laboratory engines.
pub type Result<T> = std::result::Result<T, DbError>;

/// Stable, coarse error classes suitable for harness reporting.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum ErrorClass {
    /// The requested operation violates common logical limits.
    InvalidInput,
    /// An operating-system I/O operation failed.
    Io,
    /// Persistent bytes violate the declared format.
    Corruption,
    /// A supported magic value had an unsupported version.
    UnsupportedVersion,
    /// A previous append failed and the engine must be reopened.
    Poisoned,
}

/// Errors shared across the currently implemented engines.
#[derive(Debug, Error)]
pub enum DbError {
    /// An operation or workload violates common semantics.
    #[error("invalid input: {0}")]
    InvalidInput(String),
    /// An operating-system I/O operation failed.
    #[error("I/O error: {0}")]
    Io(#[from] io::Error),
    /// Persistent storage is malformed or internally inconsistent.
    #[error("corrupt storage at byte offset {offset}: {reason}")]
    Corruption {
        /// Byte offset at which validation failed.
        offset: u64,
        /// Human-readable validation failure.
        reason: String,
    },
    /// A file has valid magic but a version this build does not understand.
    #[error("unsupported {format} version {found}; this build supports version {supported}")]
    UnsupportedVersion {
        /// Format being decoded.
        format: &'static str,
        /// Version found on disk or in a workload.
        found: u64,
        /// Version supported by this build.
        supported: u64,
    },
    /// A persistent engine cannot safely continue after an ambiguous write failure.
    #[error(
        "engine is poisoned by a previous persistent-write failure; reopen it before continuing"
    )]
    Poisoned,
}

impl DbError {
    /// Returns a stable high-level class without discarding the detailed error.
    #[must_use]
    pub fn class(&self) -> ErrorClass {
        match self {
            Self::InvalidInput(_) => ErrorClass::InvalidInput,
            Self::Io(_) => ErrorClass::Io,
            Self::Corruption { .. } => ErrorClass::Corruption,
            Self::UnsupportedVersion { .. } => ErrorClass::UnsupportedVersion,
            Self::Poisoned => ErrorClass::Poisoned,
        }
    }
}
