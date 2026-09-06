use serde::{Deserialize, Serialize};

use crate::{engine::validate_key, engine::validate_key_value, ByteString, DbError, Result};
use crate::{WorkloadStep, MAX_VALUE_BYTES};

/// JSON workload schema version supported by this build.
pub const WORKLOAD_FORMAT_VERSION: u16 = 1;
/// Defensive upper bound for deserialized workload steps.
pub const MAX_WORKLOAD_STEPS: usize = 1_000_000;

/// A normalized, reproducible sequence of logical and lifecycle actions.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Workload {
    /// Workload JSON schema version.
    pub format_version: u16,
    /// Generator seed, when the workload was generated rather than hand-authored.
    pub seed: Option<u64>,
    /// Ordered actions.
    pub steps: Vec<WorkloadStep>,
}

impl Workload {
    /// Validates schema version, size bounds, and all common operation limits.
    pub fn validate(&self) -> Result<()> {
        if self.format_version != WORKLOAD_FORMAT_VERSION {
            return Err(DbError::UnsupportedVersion {
                format: "workload",
                found: u64::from(self.format_version),
                supported: u64::from(WORKLOAD_FORMAT_VERSION),
            });
        }
        if self.steps.len() > MAX_WORKLOAD_STEPS {
            return Err(DbError::InvalidInput(format!(
                "workload has {} steps; maximum is {MAX_WORKLOAD_STEPS}",
                self.steps.len()
            )));
        }
        for step in &self.steps {
            match step {
                WorkloadStep::Put { key, value } => {
                    validate_key_value(key.as_slice(), value.as_slice())?;
                }
                WorkloadStep::Get { key } | WorkloadStep::Delete { key } => {
                    validate_key(key.as_slice())?;
                }
                WorkloadStep::Reopen => {}
            }
        }
        Ok(())
    }
}

/// Inputs for the deliberately stable SplitMix64 workload generator.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct GeneratorConfig {
    /// Seed recorded in the resulting workload.
    pub seed: u64,
    /// Number of logical KV actions, excluding inserted reopen actions.
    pub operations: u32,
    /// Number of reusable keys. Key id zero maps to the empty binary key.
    pub key_space: u32,
    /// Maximum generated value size, inclusive.
    pub max_value_bytes: u32,
    /// Insert a reopen after this many logical actions.
    pub reopen_every: Option<u32>,
}

/// Generates a deterministic workload with 50% puts, 30% gets, and 20% deletes.
pub fn generate_workload(config: GeneratorConfig) -> Result<Workload> {
    if config.key_space == 0 {
        return Err(DbError::InvalidInput(
            "generator key_space must be greater than zero".to_owned(),
        ));
    }
    if usize::try_from(config.max_value_bytes).unwrap_or(usize::MAX) > MAX_VALUE_BYTES {
        return Err(DbError::InvalidInput(format!(
            "generator max_value_bytes is {}; maximum is {MAX_VALUE_BYTES}",
            config.max_value_bytes
        )));
    }
    if config.reopen_every == Some(0) {
        return Err(DbError::InvalidInput(
            "generator reopen_every must be greater than zero".to_owned(),
        ));
    }

    let reopen_count = config
        .reopen_every
        .map_or(0, |every| config.operations / every);
    let capacity = u64::from(config.operations)
        .checked_add(u64::from(reopen_count))
        .and_then(|count| usize::try_from(count).ok())
        .ok_or_else(|| DbError::InvalidInput("generated workload is too large".to_owned()))?;
    if capacity > MAX_WORKLOAD_STEPS {
        return Err(DbError::InvalidInput(format!(
            "generated workload would have {capacity} steps; maximum is {MAX_WORKLOAD_STEPS}"
        )));
    }

    let mut random = SplitMix64::new(config.seed);
    let mut steps = Vec::with_capacity(capacity);
    for operation_index in 0..config.operations {
        let key_id = random.bounded(u64::from(config.key_space)) as u32;
        let key = generated_key(key_id);
        match random.bounded(100) {
            0..=49 => {
                let length = random.bounded(u64::from(config.max_value_bytes) + 1) as usize;
                let mut value = vec![0_u8; length];
                random.fill(&mut value);
                steps.push(WorkloadStep::Put {
                    key: ByteString::from(key),
                    value: ByteString::from(value),
                });
            }
            50..=79 => steps.push(WorkloadStep::Get {
                key: ByteString::from(key),
            }),
            _ => steps.push(WorkloadStep::Delete {
                key: ByteString::from(key),
            }),
        }

        if config
            .reopen_every
            .is_some_and(|every| (operation_index + 1) % every == 0)
        {
            steps.push(WorkloadStep::Reopen);
        }
    }

    Ok(Workload {
        format_version: WORKLOAD_FORMAT_VERSION,
        seed: Some(config.seed),
        steps,
    })
}

fn generated_key(key_id: u32) -> Vec<u8> {
    if key_id == 0 {
        Vec::new()
    } else {
        key_id.to_le_bytes().to_vec()
    }
}

/// Small specified PRNG chosen so workload generation does not depend on a third-party RNG API.
struct SplitMix64 {
    state: u64,
}

impl SplitMix64 {
    const fn new(seed: u64) -> Self {
        Self { state: seed }
    }

    fn next(&mut self) -> u64 {
        self.state = self.state.wrapping_add(0x9e37_79b9_7f4a_7c15);
        let mut value = self.state;
        value = (value ^ (value >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
        value = (value ^ (value >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
        value ^ (value >> 31)
    }

    fn bounded(&mut self, upper_exclusive: u64) -> u64 {
        self.next() % upper_exclusive
    }

    fn fill(&mut self, bytes: &mut [u8]) {
        for chunk in bytes.chunks_mut(8) {
            let random = self.next().to_le_bytes();
            chunk.copy_from_slice(&random[..chunk.len()]);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{generate_workload, GeneratorConfig};

    #[test]
    fn generator_is_repeatable_and_records_seed() {
        let config = GeneratorConfig {
            seed: 0x5eed,
            operations: 100,
            key_space: 8,
            max_value_bytes: 32,
            reopen_every: Some(7),
        };
        let first = generate_workload(config).expect("generate workload");
        let second = generate_workload(config).expect("regenerate workload");
        assert_eq!(first, second);
        assert_eq!(first.seed, Some(0x5eed));
        assert_eq!(first.steps.len(), 114);
    }
}
