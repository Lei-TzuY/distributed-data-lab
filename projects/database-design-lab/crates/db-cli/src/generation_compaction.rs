use std::path::Path;

use db_core::DbError;
#[cfg(unix)]
use db_storage_log::{InspectionReport, LogEngine};
use serde::Serialize;
use thiserror::Error;

#[cfg(unix)]
use crate::generation_directory::{canonical_generation_name, verify_generation_directory};
use crate::generation_directory::{GenerationDirectoryError, GenerationVerificationSummary};
#[cfg(unix)]
use crate::generation_lock::acquire_generation_writer_lease;
use crate::generation_lock::GenerationWriterLockError;
#[cfg(all(unix, not(test)))]
use crate::generation_publication::publish_generation_marker;
#[cfg(all(test, unix))]
use crate::generation_publication::{
    publish_generation_marker_with_fault, GenerationPublicationFaultPoint,
};
use crate::generation_publication::{GenerationPublicationError, GenerationPublicationSummary};
#[cfg(unix)]
use crate::generation_reservation::reserve_next_generation;
use crate::generation_reservation::{GenerationReservationError, GenerationReservationSummary};
#[cfg(unix)]
use crate::log_compaction::compact_log_to_fresh_file;
use crate::log_compaction::{LogCompactionError, LogCompactionReport};

pub const OFFLINE_GENERATION_COMPACT_SWITCH_PROTOCOL: &str =
    "append_log_offline_generation_compact_switch_unix_v2";

#[cfg(all(test, unix))]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum OfflineGenerationCompactSwitchFaultPoint {
    CompactCandidatePublished,
    CriticalSectionVerified,
    GenerationDurable,
    StagingMarkerPartiallyWritten,
    StagingMarkerDurable,
    FinalMarkerLinked,
    FinalDirectoryDurable,
    MarkerPublicationCompleted,
}

#[cfg(all(test, unix))]
impl OfflineGenerationCompactSwitchFaultPoint {
    const fn label(self) -> &'static str {
        match self {
            Self::CompactCandidatePublished => "compact_candidate_published",
            Self::CriticalSectionVerified => "critical_section_verified",
            Self::GenerationDurable => "generation_durable",
            Self::StagingMarkerPartiallyWritten => "staging_marker_partially_written",
            Self::StagingMarkerDurable => "staging_marker_durable",
            Self::FinalMarkerLinked => "final_marker_linked",
            Self::FinalDirectoryDurable => "final_directory_durable",
            Self::MarkerPublicationCompleted => "marker_publication_completed",
        }
    }

    const fn publication_fault(self) -> Option<GenerationPublicationFaultPoint> {
        match self {
            Self::GenerationDurable => Some(GenerationPublicationFaultPoint::GenerationDurable),
            Self::StagingMarkerPartiallyWritten => {
                Some(GenerationPublicationFaultPoint::StagingMarkerPartiallyWritten)
            }
            Self::StagingMarkerDurable => {
                Some(GenerationPublicationFaultPoint::StagingMarkerDurable)
            }
            Self::FinalMarkerLinked => Some(GenerationPublicationFaultPoint::FinalMarkerLinked),
            Self::FinalDirectoryDurable => {
                Some(GenerationPublicationFaultPoint::FinalDirectoryDurable)
            }
            Self::CompactCandidatePublished
            | Self::CriticalSectionVerified
            | Self::MarkerPublicationCompleted => None,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct OfflineGenerationCompactSwitchSummary {
    pub protocol: &'static str,
    pub old_generation: u64,
    pub new_generation: u64,
    pub old_generation_log: String,
    pub new_generation_log: String,
    pub reservation: GenerationReservationSummary,
    pub compaction: LogCompactionReport,
    pub publication: GenerationPublicationSummary,
    pub final_generation: GenerationVerificationSummary,
}

#[derive(Debug, Error)]
pub enum OfflineGenerationCompactSwitchError {
    #[error("offline generation compact switch is unsupported on this platform; no artifact was written")]
    UnsupportedPlatform,
    #[error(transparent)]
    Directory(#[from] GenerationDirectoryError),
    #[error(transparent)]
    WriterLock(#[from] GenerationWriterLockError),
    #[error(transparent)]
    Reservation(#[from] GenerationReservationError),
    #[error(transparent)]
    Database(#[from] DbError),
    #[error(transparent)]
    Compaction(#[from] LogCompactionError),
    #[error(transparent)]
    Publication(#[from] GenerationPublicationError),
    #[error(
        "reserved generation {reserved_generation} is no longer newer than authoritative generation {authoritative_generation}; retry to reserve a fresh generation id"
    )]
    ReservedGenerationObsolete {
        reserved_generation: u64,
        authoritative_generation: u64,
    },
    #[error(
        "authoritative source changed while offline compaction was in progress; generation {orphan_generation} remains uncommitted and must not be treated as authoritative"
    )]
    SourceChanged { orphan_generation: u64 },
    #[error("new compact generation {generation} changed before marker publication")]
    TargetChanged { generation: u64 },
    #[error(
        "post-publication verification selected generation {found}, expected newly committed generation {expected}"
    )]
    FinalAuthority { found: u64, expected: u64 },
    #[error("new authoritative generation {generation} changed after marker publication")]
    FinalState { generation: u64 },
    #[cfg(test)]
    #[error("injected offline generation compact-switch fault at {point}")]
    InjectedFault { point: &'static str },
}

/// Offline authoritative compact switch for a generation directory.
///
/// The operation first durably reserves its generation id under the shared writer lease, then releases
/// the lease while building the expensive compact copy. The retained reservation permanently advances
/// the allocation frontier even if candidate construction or later publication fails. Immediately before
/// authority can change, the function reacquires the cooperative cross-process writer lease, re-verifies
/// the old authority and complete source state, verifies the compact candidate, publishes the marker, and
/// verifies the new authority while still holding the lease. `GenerationLogEngine` operations use the same
/// lease, closing their final check-to-publication race. Standalone `LogEngine` constructors reject
/// canonical generation names; callers that deliberately invoke the explicit managed-generation
/// constructor without the lease remain outside this cooperative coordination contract.
pub fn compact_switch_generation_offline(
    directory: &Path,
) -> Result<OfflineGenerationCompactSwitchSummary, OfflineGenerationCompactSwitchError> {
    #[cfg(unix)]
    {
        compact_switch_generation_offline_impl(
            directory,
            |_| Ok(()),
            #[cfg(test)]
            None,
        )
    }

    #[cfg(not(unix))]
    {
        let _ = directory;
        Err(OfflineGenerationCompactSwitchError::UnsupportedPlatform)
    }
}

#[cfg(unix)]
fn compact_switch_generation_offline_impl<F>(
    directory: &Path,
    after_compaction: F,
    #[cfg(test)] fault: Option<OfflineGenerationCompactSwitchFaultPoint>,
) -> Result<OfflineGenerationCompactSwitchSummary, OfflineGenerationCompactSwitchError>
where
    F: FnOnce(&Path) -> Result<(), OfflineGenerationCompactSwitchError>,
{
    let reservation = reserve_next_generation(directory)?;
    let before = verify_generation_directory(directory)?;
    let old_generation = before.summary().authoritative_generation;
    if reservation.generation <= old_generation {
        return Err(
            OfflineGenerationCompactSwitchError::ReservedGenerationObsolete {
                reserved_generation: reservation.generation,
                authoritative_generation: old_generation,
            },
        );
    }
    let old_generation_log = before.summary().authoritative_log.clone();
    let source_path = before.authoritative_log_path();
    let source_state = LogEngine::inspect(&source_path, true)?;
    let source_authority = SourceAuthorityWitness::from_summary(before.summary());
    let new_generation = reservation.generation;
    let new_generation_log = canonical_generation_name(new_generation);
    let new_path = before.directory().join(&new_generation_log);

    let compaction = compact_log_to_fresh_file(&source_path, &new_path)?;
    #[cfg(test)]
    inject_fault(
        fault,
        OfflineGenerationCompactSwitchFaultPoint::CompactCandidatePublished,
    )?;
    after_compaction(&source_path)?;

    // Only the authority-changing critical section is exclusive. A compliant routed writer may run
    // during compact-copy construction; if it did, this locked recheck detects the drift and leaves
    // the candidate as a harmless uncommitted orphan. Its durable reservation keeps the id retired.
    let lease = acquire_generation_writer_lease(before.directory())?;
    let before_publication = verify_generation_directory(lease.directory())?;
    let current_authority = SourceAuthorityWitness::from_summary(before_publication.summary());
    let current_source_state = LogEngine::inspect(&source_path, true)?;
    if current_authority != source_authority || current_source_state != source_state {
        return Err(OfflineGenerationCompactSwitchError::SourceChanged {
            orphan_generation: new_generation,
        });
    }

    let compact_state = LogEngine::inspect(&new_path, true)?;
    validate_compact_state(new_generation, &source_state, &compact_state)?;
    #[cfg(test)]
    inject_fault(
        fault,
        OfflineGenerationCompactSwitchFaultPoint::CriticalSectionVerified,
    )?;

    #[cfg(not(test))]
    let publication = publish_generation_marker(lease.directory(), new_generation)?;
    #[cfg(test)]
    let publication = publish_generation_marker_with_fault(
        lease.directory(),
        new_generation,
        fault.and_then(OfflineGenerationCompactSwitchFaultPoint::publication_fault),
    )?;
    #[cfg(test)]
    inject_fault(
        fault,
        OfflineGenerationCompactSwitchFaultPoint::MarkerPublicationCompleted,
    )?;
    let final_verified = verify_generation_directory(lease.directory())?;
    if final_verified.summary().authoritative_generation != new_generation {
        return Err(OfflineGenerationCompactSwitchError::FinalAuthority {
            found: final_verified.summary().authoritative_generation,
            expected: new_generation,
        });
    }

    let final_state = LogEngine::inspect(&new_path, true)?;
    if final_state != compact_state {
        return Err(OfflineGenerationCompactSwitchError::FinalState {
            generation: new_generation,
        });
    }

    Ok(OfflineGenerationCompactSwitchSummary {
        protocol: OFFLINE_GENERATION_COMPACT_SWITCH_PROTOCOL,
        old_generation,
        new_generation,
        old_generation_log,
        new_generation_log,
        reservation,
        compaction,
        publication,
        final_generation: final_verified.summary().clone(),
    })
}

#[cfg(all(test, unix))]
fn inject_fault(
    selected: Option<OfflineGenerationCompactSwitchFaultPoint>,
    current: OfflineGenerationCompactSwitchFaultPoint,
) -> Result<(), OfflineGenerationCompactSwitchError> {
    if selected == Some(current) {
        return Err(OfflineGenerationCompactSwitchError::InjectedFault {
            point: current.label(),
        });
    }
    Ok(())
}

#[cfg(unix)]
#[derive(Debug, Clone, PartialEq, Eq)]
struct SourceAuthorityWitness {
    authoritative_generation: u64,
    committed_prefix: crate::generation_marker::CommittedPrefix,
    log_verification: db_storage_log::VerificationReport,
}

#[cfg(unix)]
impl SourceAuthorityWitness {
    fn from_summary(summary: &GenerationVerificationSummary) -> Self {
        Self {
            authoritative_generation: summary.authoritative_generation,
            committed_prefix: summary.committed_prefix,
            log_verification: summary.log_verification.clone(),
        }
    }
}

#[cfg(unix)]
fn validate_compact_state(
    generation: u64,
    source: &InspectionReport,
    compacted: &InspectionReport,
) -> Result<(), OfflineGenerationCompactSwitchError> {
    if compacted.verification.recoverable_tail.is_some()
        || compacted.verification.record_count != compacted.verification.live_keys as u64
        || compacted.verification.live_keys != source.verification.live_keys
        || compacted.entries != source.entries
    {
        return Err(OfflineGenerationCompactSwitchError::TargetChanged { generation });
    }
    Ok(())
}

#[cfg(all(test, unix))]
mod tests {
    use std::fs;
    use std::path::PathBuf;

    use db_core::KvEngine;
    use tempfile::{tempdir, TempDir};

    use super::*;
    use crate::generation_directory::{
        canonical_marker_name, canonical_staging_marker_name, verify_generation_directory,
    };
    use crate::generation_lock::generation_writer_lock_path;
    use crate::generation_marker::{decode_commit_marker, COMMIT_MARKER_LEN};
    use crate::generation_prefix::verify_committed_prefix;
    use crate::generation_publication::publish_generation_marker;

    #[derive(Debug, Clone, Copy)]
    struct FaultCase {
        point: OfflineGenerationCompactSwitchFaultPoint,
        expected_authority: u64,
        expected_staging_bytes: Option<u64>,
        final_marker_present: bool,
    }

    fn compact_switch_fixture() -> (TempDir, PathBuf, InspectionReport) {
        let root = tempdir().expect("temporary root");
        let directory = root.path().join("generations");
        fs::create_dir(&directory).expect("create generation directory");
        let source = directory.join(canonical_generation_name(1));
        {
            let mut engine = LogEngine::create_new_managed_generation(&source)
                .expect("create source generation");
            engine.put(b"a", b"one").expect("put a one");
            engine.put(b"a", b"two").expect("overwrite a");
            engine.put(b"deleted", b"value").expect("put deleted key");
            engine.delete(b"deleted").expect("delete key");
            engine.delete(b"missing").expect("delete missing key");
            engine.put(b"", b"").expect("put empty key and value");
            engine
                .put(&[0, 0xff, 0x80], &[0xff, 0, 0x7f])
                .expect("put binary key and value");
        }
        publish_generation_marker(&directory, 1).expect("publish source generation");
        let source_state = LogEngine::inspect(&source, true).expect("inspect source generation");
        (root, directory, source_state)
    }

    fn run_fault(
        directory: &Path,
        point: OfflineGenerationCompactSwitchFaultPoint,
    ) -> OfflineGenerationCompactSwitchError {
        compact_switch_generation_offline_impl(directory, |_| Ok(()), Some(point))
            .expect_err("selected compact-switch point must fail")
    }

    fn assert_selected_fault(
        error: &OfflineGenerationCompactSwitchError,
        point: OfflineGenerationCompactSwitchFaultPoint,
    ) {
        match point.publication_fault() {
            Some(_) => assert!(
                matches!(
                    error,
                    OfflineGenerationCompactSwitchError::Publication(
                        GenerationPublicationError::Invalid(message)
                    ) if message.contains(point.label())
                ),
                "fault {point:?} returned the wrong error: {error}"
            ),
            None => assert!(
                matches!(
                    error,
                    OfflineGenerationCompactSwitchError::InjectedFault {
                        point: actual
                    } if *actual == point.label()
                ),
                "fault {point:?} returned the wrong error: {error}"
            ),
        }
    }

    fn assert_recovered_logical_state(
        directory: &Path,
        source_state: &InspectionReport,
        expected_authority: u64,
    ) {
        let verified =
            verify_generation_directory(directory).expect("recover generation directory");
        assert_eq!(
            verified.summary().authoritative_generation,
            expected_authority
        );
        let recovered = LogEngine::inspect(verified.authoritative_log_path(), true)
            .expect("inspect recovered authority");
        assert_eq!(recovered.entries, source_state.entries);
    }

    #[test]
    fn composed_fault_matrix_recovers_exact_old_or_new_logical_state() {
        let cases = [
            FaultCase {
                point: OfflineGenerationCompactSwitchFaultPoint::CompactCandidatePublished,
                expected_authority: 1,
                expected_staging_bytes: None,
                final_marker_present: false,
            },
            FaultCase {
                point: OfflineGenerationCompactSwitchFaultPoint::CriticalSectionVerified,
                expected_authority: 1,
                expected_staging_bytes: None,
                final_marker_present: false,
            },
            FaultCase {
                point: OfflineGenerationCompactSwitchFaultPoint::GenerationDurable,
                expected_authority: 1,
                expected_staging_bytes: None,
                final_marker_present: false,
            },
            FaultCase {
                point: OfflineGenerationCompactSwitchFaultPoint::StagingMarkerPartiallyWritten,
                expected_authority: 1,
                expected_staging_bytes: Some((COMMIT_MARKER_LEN / 2) as u64),
                final_marker_present: false,
            },
            FaultCase {
                point: OfflineGenerationCompactSwitchFaultPoint::StagingMarkerDurable,
                expected_authority: 1,
                expected_staging_bytes: Some(COMMIT_MARKER_LEN as u64),
                final_marker_present: false,
            },
            FaultCase {
                point: OfflineGenerationCompactSwitchFaultPoint::FinalMarkerLinked,
                expected_authority: 2,
                expected_staging_bytes: Some(COMMIT_MARKER_LEN as u64),
                final_marker_present: true,
            },
            FaultCase {
                point: OfflineGenerationCompactSwitchFaultPoint::FinalDirectoryDurable,
                expected_authority: 2,
                expected_staging_bytes: Some(COMMIT_MARKER_LEN as u64),
                final_marker_present: true,
            },
            FaultCase {
                point: OfflineGenerationCompactSwitchFaultPoint::MarkerPublicationCompleted,
                expected_authority: 2,
                expected_staging_bytes: None,
                final_marker_present: true,
            },
        ];

        for case in cases {
            let (_root, directory, source_state) = compact_switch_fixture();
            let error = run_fault(&directory, case.point);
            assert_selected_fault(&error, case.point);

            let new_generation = directory.join(canonical_generation_name(2));
            let final_marker = directory.join(canonical_marker_name(2));
            let staging_marker = directory.join(canonical_staging_marker_name(2));
            assert!(directory.join(canonical_generation_name(1)).is_file());
            assert!(directory.join(canonical_marker_name(1)).is_file());
            assert!(new_generation.is_file(), "fault {:?}", case.point);
            assert_eq!(
                final_marker.is_file(),
                case.final_marker_present,
                "fault {:?}",
                case.point
            );
            match case.expected_staging_bytes {
                Some(expected) => {
                    let staging_bytes = fs::read(&staging_marker).expect("retained staging marker");
                    assert_eq!(
                        staging_bytes.len() as u64,
                        expected,
                        "fault {:?}",
                        case.point
                    );
                    if expected == COMMIT_MARKER_LEN as u64 {
                        let staged = decode_commit_marker(&staging_bytes, 2)
                            .expect("complete staging marker must validate");
                        let compact_verification =
                            LogEngine::verify(&new_generation).expect("verify compact generation");
                        let proof =
                            verify_committed_prefix(&new_generation, staged.committed_prefix)
                                .expect("staging marker must prove the compact generation");
                        assert_eq!(proof, compact_verification, "fault {:?}", case.point);
                    }
                }
                None => assert!(!staging_marker.exists(), "fault {:?}", case.point),
            }

            let verified =
                verify_generation_directory(&directory).expect("verify interrupted switch state");
            assert_eq!(
                verified.summary().authoritative_generation,
                case.expected_authority,
                "fault {:?}",
                case.point
            );
            assert_eq!(
                verified.summary().reservation_generation_ids,
                vec![2],
                "fault {:?} must retain its durable reservation",
                case.point
            );
            assert_eq!(
                verified.summary().uncommitted_generation_ids,
                if case.expected_authority == 1 {
                    vec![2]
                } else {
                    Vec::new()
                },
                "fault {:?}",
                case.point
            );
            assert_eq!(
                verified.summary().staging_marker_generation_ids,
                if case.expected_staging_bytes.is_some() {
                    vec![2]
                } else {
                    Vec::new()
                },
                "fault {:?}",
                case.point
            );
            assert_recovered_logical_state(&directory, &source_state, case.expected_authority);

            let lock_path =
                generation_writer_lock_path(&directory).expect("derive writer lock path");
            assert!(
                !lock_path.exists(),
                "gracefully injected fault {:?} must release its cooperative lease",
                case.point
            );
        }
    }

    #[test]
    fn modeled_loss_of_unsynced_final_marker_recovers_old_generation() {
        let (_root, directory, source_state) = compact_switch_fixture();
        let error = run_fault(
            &directory,
            OfflineGenerationCompactSwitchFaultPoint::FinalMarkerLinked,
        );
        assert_selected_fault(
            &error,
            OfflineGenerationCompactSwitchFaultPoint::FinalMarkerLinked,
        );

        assert_recovered_logical_state(&directory, &source_state, 2);
        fs::remove_file(directory.join(canonical_marker_name(2)))
            .expect("model loss of the unsynchronized final-marker directory entry");

        let recovered =
            verify_generation_directory(&directory).expect("recover modeled pre-barrier state");
        assert_eq!(recovered.summary().authoritative_generation, 1);
        assert_eq!(recovered.summary().reservation_generation_ids, vec![2]);
        assert_eq!(recovered.summary().uncommitted_generation_ids, vec![2]);
        assert_eq!(recovered.summary().staging_marker_generation_ids, vec![2]);
        assert_recovered_logical_state(&directory, &source_state, 1);
    }

    #[test]
    fn source_drift_after_compaction_never_publishes_stale_generation() {
        let root = tempdir().expect("temporary root");
        let directory = root.path().join("generations");
        fs::create_dir(&directory).expect("create generation directory");
        let source = directory.join(canonical_generation_name(1));
        {
            let mut engine = LogEngine::create_new_managed_generation(&source)
                .expect("create source generation");
            engine.put(b"a", b"one").expect("put initial value");
        }
        publish_generation_marker(&directory, 1).expect("publish source generation");

        let error = compact_switch_generation_offline_impl(
            &directory,
            |source_path| {
                let mut engine = LogEngine::open_managed_generation(source_path)
                    .expect("open source for injected protocol-violating drift");
                engine.put(b"late", b"write").expect("inject late write");
                Ok(())
            },
            None,
        )
        .expect_err("source drift must fail the switch");

        assert!(matches!(
            error,
            OfflineGenerationCompactSwitchError::SourceChanged {
                orphan_generation: 2
            }
        ));
        assert!(directory.join(canonical_generation_name(2)).is_file());
        assert!(!directory.join(canonical_marker_name(2)).exists());
        let verified = verify_generation_directory(&directory).expect("verify old authority");
        assert_eq!(verified.summary().authoritative_generation, 1);
        assert_eq!(verified.summary().reservation_generation_ids, vec![2]);
    }
}
