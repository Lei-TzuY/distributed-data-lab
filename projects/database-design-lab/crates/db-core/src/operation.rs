use serde::{Deserialize, Serialize};

use crate::ByteString;

/// One logical KV action or one explicit engine-lifecycle action.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "op", rename_all = "snake_case")]
pub enum WorkloadStep {
    /// Set `key` to `value`, replacing any prior value.
    Put {
        /// Arbitrary binary key encoded as hexadecimal in JSON.
        key: ByteString,
        /// Arbitrary binary value encoded as hexadecimal in JSON.
        value: ByteString,
    },
    /// Read the current value for `key`.
    Get {
        /// Arbitrary binary key encoded as hexadecimal in JSON.
        key: ByteString,
    },
    /// Remove `key` if it exists. A persistent engine still records a tombstone for a miss.
    Delete {
        /// Arbitrary binary key encoded as hexadecimal in JSON.
        key: ByteString,
    },
    /// Close and reopen the engine without changing logical state.
    Reopen,
}

/// Observable result of a workload step.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "result", rename_all = "snake_case")]
pub enum Outcome {
    /// Result of a put, including the value that was replaced.
    Put {
        /// Previous value, or `None` if the key was absent.
        previous: Option<ByteString>,
    },
    /// Result of a point lookup.
    Get {
        /// Current value, or `None` if the key is absent.
        value: Option<ByteString>,
    },
    /// Result of a delete.
    Delete {
        /// Removed value, or `None` if the key was absent.
        previous: Option<ByteString>,
    },
    /// Successful lifecycle boundary.
    Reopened,
}
