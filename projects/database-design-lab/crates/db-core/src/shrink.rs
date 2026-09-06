use crate::{DbError, Workload};

/// Minimizes a valid workload while preserving a caller-defined failure predicate.
///
/// The predicate must be deterministic for a given workload. The minimizer first applies a classic
/// chunk-removal delta-debugging pass and then performs single-step deletion until the result is
/// 1-minimal: removing any one remaining step no longer reproduces the target failure.
///
/// Metadata is preserved verbatim. In particular, a generated workload keeps its original seed even
/// though the minimized step vector is no longer the direct generator output; the committed minimized
/// regression therefore retains provenance without pretending it can be regenerated from the seed alone.
pub fn minimize_failing_workload<F, E>(
    workload: &Workload,
    mut reproduces_failure: F,
) -> std::result::Result<Workload, E>
where
    F: FnMut(&Workload) -> std::result::Result<bool, E>,
    E: From<DbError>,
{
    workload.validate().map_err(E::from)?;
    if !reproduces_failure(workload)? {
        return Err(E::from(DbError::InvalidInput(
            "workload minimization requires the original workload to reproduce the target failure"
                .to_owned(),
        )));
    }

    let mut current = workload.clone();
    let mut granularity = 2_usize;

    while current.steps.len() >= 2 {
        let length = current.steps.len();
        let chunk_size = length.div_ceil(granularity);
        let mut reduced = false;
        let mut start = 0_usize;

        while start < length {
            let end = start.saturating_add(chunk_size).min(length);
            if end - start == length {
                break;
            }
            let candidate = without_range(&current, start, end);
            if reproduces_failure(&candidate)? {
                current = candidate;
                granularity = granularity.saturating_sub(1).max(2);
                reduced = true;
                break;
            }
            start = end;
        }

        if reduced {
            continue;
        }
        if granularity >= length {
            break;
        }
        granularity = granularity.saturating_mul(2).min(length);
    }

    let mut index = 0_usize;
    while index < current.steps.len() {
        let candidate = without_range(&current, index, index + 1);
        if reproduces_failure(&candidate)? {
            current = candidate;
            index = 0;
        } else {
            index += 1;
        }
    }

    Ok(current)
}

fn without_range(workload: &Workload, start: usize, end: usize) -> Workload {
    debug_assert!(start <= end);
    debug_assert!(end <= workload.steps.len());
    let mut steps = Vec::with_capacity(workload.steps.len().saturating_sub(end - start));
    steps.extend_from_slice(&workload.steps[..start]);
    steps.extend_from_slice(&workload.steps[end..]);
    Workload {
        format_version: workload.format_version,
        seed: workload.seed,
        steps,
    }
}

#[cfg(test)]
mod tests {
    use crate::{ByteString, DbError, WorkloadStep, WORKLOAD_FORMAT_VERSION};

    use super::minimize_failing_workload;
    use crate::Workload;

    fn get(key: &[u8]) -> WorkloadStep {
        WorkloadStep::Get {
            key: ByteString::from(key.to_vec()),
        }
    }

    fn put(key: &[u8], value: &[u8]) -> WorkloadStep {
        WorkloadStep::Put {
            key: ByteString::from(key.to_vec()),
            value: ByteString::from(value.to_vec()),
        }
    }

    #[test]
    fn shrinks_to_one_required_step_and_preserves_provenance() {
        let workload = Workload {
            format_version: WORKLOAD_FORMAT_VERSION,
            seed: Some(42),
            steps: vec![
                get(b"a"),
                put(b"b", b"one"),
                WorkloadStep::Reopen,
                get(b"target"),
                put(b"c", b"two"),
                get(b"d"),
            ],
        };

        let minimized = minimize_failing_workload(&workload, |candidate| {
            Ok::<_, DbError>(candidate.steps.iter().any(
                |step| matches!(step, WorkloadStep::Get { key } if key.as_slice() == b"target"),
            ))
        })
        .expect("minimize workload");

        assert_eq!(minimized.seed, Some(42));
        assert_eq!(minimized.steps, vec![get(b"target")]);
    }

    #[test]
    fn preserves_interacting_ordered_steps_and_is_one_minimal() {
        let workload = Workload {
            format_version: WORKLOAD_FORMAT_VERSION,
            seed: None,
            steps: vec![
                get(b"noise-a"),
                put(b"key", b"value"),
                get(b"noise-b"),
                get(b"key"),
                WorkloadStep::Reopen,
            ],
        };
        let predicate = |candidate: &Workload| {
            let put_index = candidate.steps.iter().position(
                |step| matches!(step, WorkloadStep::Put { key, .. } if key.as_slice() == b"key"),
            );
            let get_index = candidate.steps.iter().position(
                |step| matches!(step, WorkloadStep::Get { key } if key.as_slice() == b"key"),
            );
            Ok::<_, DbError>(matches!((put_index, get_index), (Some(put), Some(get)) if put < get))
        };

        let minimized = minimize_failing_workload(&workload, predicate).expect("minimize workload");
        assert_eq!(minimized.steps, vec![put(b"key", b"value"), get(b"key")]);

        for index in 0..minimized.steps.len() {
            let mut candidate = minimized.clone();
            candidate.steps.remove(index);
            assert!(
                minimize_failing_workload(&candidate, |probe| {
                    let has_put = probe.steps.iter().any(|step| {
                        matches!(step, WorkloadStep::Put { key, .. } if key.as_slice() == b"key")
                    });
                    let has_get = probe.steps.iter().any(|step| {
                        matches!(step, WorkloadStep::Get { key } if key.as_slice() == b"key")
                    });
                    Ok::<_, DbError>(has_put && has_get)
                })
                .is_err(),
                "removing step {index} must stop reproduction"
            );
        }
    }

    #[test]
    fn rejects_an_original_workload_that_does_not_reproduce() {
        let workload = Workload {
            format_version: WORKLOAD_FORMAT_VERSION,
            seed: None,
            steps: vec![get(b"healthy")],
        };
        let error = minimize_failing_workload(&workload, |_| Ok::<_, DbError>(false))
            .expect_err("non-reproducing input must be rejected");
        assert!(matches!(error, DbError::InvalidInput(_)));
    }
}
