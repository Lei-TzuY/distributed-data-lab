use std::collections::BTreeMap;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum LogHealth {
    Missing,
    Clean,
    RecoverableTail { committed_prefix_proven: bool },
    Corrupt,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct GenerationState {
    id: u64,
    commit_marker_durable: bool,
    log_health: LogHealth,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum RecoveryDecision {
    OpenGeneration { id: u64, repair_tail_on_open: bool },
    FailClosed(FailureReason),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum FailureReason {
    NoCommittedGeneration,
    DuplicateGenerationId,
    HighestCommittedLogMissing,
    HighestCommittedLogCorrupt,
    HighestCommittedPrefixUnproven,
}

fn select_authoritative_generation(states: &[GenerationState]) -> RecoveryDecision {
    let mut by_id = BTreeMap::new();
    for state in states {
        if by_id.insert(state.id, *state).is_some() {
            return RecoveryDecision::FailClosed(FailureReason::DuplicateGenerationId);
        }
    }

    let Some(authoritative) = by_id
        .values()
        .rev()
        .find(|state| state.commit_marker_durable)
        .copied()
    else {
        return RecoveryDecision::FailClosed(FailureReason::NoCommittedGeneration);
    };

    match authoritative.log_health {
        LogHealth::Clean => RecoveryDecision::OpenGeneration {
            id: authoritative.id,
            repair_tail_on_open: false,
        },
        LogHealth::RecoverableTail {
            committed_prefix_proven: true,
        } => RecoveryDecision::OpenGeneration {
            id: authoritative.id,
            repair_tail_on_open: true,
        },
        LogHealth::RecoverableTail {
            committed_prefix_proven: false,
        } => RecoveryDecision::FailClosed(FailureReason::HighestCommittedPrefixUnproven),
        LogHealth::Missing => {
            RecoveryDecision::FailClosed(FailureReason::HighestCommittedLogMissing)
        }
        LogHealth::Corrupt => {
            RecoveryDecision::FailClosed(FailureReason::HighestCommittedLogCorrupt)
        }
    }
}

fn generation(id: u64, committed: bool, log_health: LogHealth) -> GenerationState {
    GenerationState {
        id,
        commit_marker_durable: committed,
        log_health,
    }
}

fn open(id: u64) -> RecoveryDecision {
    RecoveryDecision::OpenGeneration {
        id,
        repair_tail_on_open: false,
    }
}

const fn recoverable_tail(committed_prefix_proven: bool) -> LogHealth {
    LogHealth::RecoverableTail {
        committed_prefix_proven,
    }
}

#[test]
fn every_valid_compaction_switch_crash_prefix_selects_exactly_old_or_new() {
    let old = generation(7, true, LogHealth::Clean);
    let cases = [
        ("before next-generation file exists", vec![old], open(7)),
        (
            "after next-generation name exists but bytes are incomplete",
            vec![old, generation(8, false, LogHealth::Corrupt)],
            open(7),
        ),
        (
            "after next-generation compact image is complete but uncommitted",
            vec![old, generation(8, false, LogHealth::Clean)],
            open(7),
        ),
        (
            "after next-generation commit marker is durable",
            vec![old, generation(8, true, LogHealth::Clean)],
            open(8),
        ),
        (
            "after old generation log cleanup",
            vec![
                generation(7, true, LogHealth::Missing),
                generation(8, true, LogHealth::Clean),
            ],
            open(8),
        ),
        (
            "after old generation marker and log cleanup",
            vec![generation(8, true, LogHealth::Clean)],
            open(8),
        ),
    ];

    for (label, state, expected) in cases {
        assert_eq!(
            select_authoritative_generation(&state),
            expected,
            "crash state {label}"
        );
    }
}

#[test]
fn higher_uncommitted_orphans_never_override_last_committed_generation() {
    let states = [
        generation(10, true, LogHealth::Clean),
        generation(11, false, LogHealth::Clean),
        generation(12, false, LogHealth::Corrupt),
        generation(13, false, LogHealth::Missing),
        generation(14, false, recoverable_tail(false)),
    ];
    assert_eq!(select_authoritative_generation(&states), open(10));
}

#[test]
fn marker_bound_recoverable_tail_remains_authoritative() {
    let states = [
        generation(3, true, LogHealth::Clean),
        generation(4, true, recoverable_tail(true)),
    ];
    assert_eq!(
        select_authoritative_generation(&states),
        RecoveryDecision::OpenGeneration {
            id: 4,
            repair_tail_on_open: true,
        }
    );
}

#[test]
fn unproven_recoverable_tail_never_becomes_authoritative() {
    let states = [
        generation(3, true, LogHealth::Clean),
        generation(4, true, recoverable_tail(false)),
    ];
    assert_eq!(
        select_authoritative_generation(&states),
        RecoveryDecision::FailClosed(FailureReason::HighestCommittedPrefixUnproven)
    );
}

#[test]
fn committed_new_generation_corruption_never_falls_back_to_old_state() {
    for (health, expected) in [
        (
            LogHealth::Missing,
            FailureReason::HighestCommittedLogMissing,
        ),
        (
            LogHealth::Corrupt,
            FailureReason::HighestCommittedLogCorrupt,
        ),
    ] {
        let states = [
            generation(21, true, LogHealth::Clean),
            generation(22, true, health),
        ];
        assert_eq!(
            select_authoritative_generation(&states),
            RecoveryDecision::FailClosed(expected)
        );
    }
}

#[test]
fn publishing_marker_before_new_log_is_proven_durable_is_an_invalid_writer_order() {
    let old = generation(30, true, LogHealth::Clean);
    for health in [
        LogHealth::Missing,
        LogHealth::Corrupt,
        recoverable_tail(false),
    ] {
        let states = [old, generation(31, true, health)];
        assert!(matches!(
            select_authoritative_generation(&states),
            RecoveryDecision::FailClosed(_)
        ));
    }
}

#[test]
fn deleting_old_generation_before_new_marker_is_durable_is_an_invalid_writer_order() {
    for old_health in [LogHealth::Missing, LogHealth::Corrupt] {
        let states = [
            generation(40, true, old_health),
            generation(41, false, LogHealth::Clean),
        ];
        assert!(matches!(
            select_authoritative_generation(&states),
            RecoveryDecision::FailClosed(_)
        ));
    }
}

#[test]
fn no_marker_and_duplicate_generation_ids_fail_closed() {
    assert_eq!(
        select_authoritative_generation(&[
            generation(1, false, LogHealth::Clean),
            generation(2, false, LogHealth::Clean),
        ]),
        RecoveryDecision::FailClosed(FailureReason::NoCommittedGeneration)
    );
    assert_eq!(
        select_authoritative_generation(&[
            generation(1, true, LogHealth::Clean),
            generation(1, false, LogHealth::Clean),
        ]),
        RecoveryDecision::FailClosed(FailureReason::DuplicateGenerationId)
    );
}

#[test]
fn arbitrary_lower_generation_damage_cannot_roll_back_a_valid_higher_commit() {
    for lower_health in [
        LogHealth::Missing,
        LogHealth::Clean,
        recoverable_tail(true),
        recoverable_tail(false),
        LogHealth::Corrupt,
    ] {
        let states = [
            generation(50, true, lower_health),
            generation(51, true, LogHealth::Clean),
        ];
        assert_eq!(select_authoritative_generation(&states), open(51));
    }
}

#[test]
fn selection_is_independent_of_directory_enumeration_order() {
    let forward = [
        generation(70, true, LogHealth::Clean),
        generation(71, false, LogHealth::Clean),
        generation(72, true, LogHealth::Clean),
    ];
    let reverse = [forward[2], forward[1], forward[0]];
    assert_eq!(select_authoritative_generation(&forward), open(72));
    assert_eq!(select_authoritative_generation(&reverse), open(72));
}

#[test]
fn successful_switch_requires_log_durability_before_marker_durability() {
    let old = generation(90, true, LogHealth::Clean);
    let writer_prefixes = [
        vec![old],
        vec![old, generation(91, false, LogHealth::Corrupt)],
        vec![old, generation(91, false, LogHealth::Clean)],
        vec![old, generation(91, true, LogHealth::Clean)],
    ];
    let expected = [open(90), open(90), open(90), open(91)];

    for (state, expected) in writer_prefixes.iter().zip(expected) {
        assert_eq!(select_authoritative_generation(state), expected);
    }
}
