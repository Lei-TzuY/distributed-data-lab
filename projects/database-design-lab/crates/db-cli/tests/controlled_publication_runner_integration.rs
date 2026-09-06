#![cfg(unix)]

use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};

use tempfile::tempdir;

const REQUIRED_BINS: [&str; 5] = [
    "db-lab-host-preflight",
    "db-lab-batch",
    "db-lab-batch-verify",
    "db-lab-publication-session",
    "db-lab-batch-analysis-bundle",
];

#[test]
fn runner_seals_successful_batch_in_expected_order() {
    let fixture = Fixture::new();
    let output = fixture.run("0");
    assert!(
        output.status.success(),
        "runner failed\nstdout:\n{}\nstderr:\n{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    assert_eq!(fixture.command_names(), expected_command_names());
    assert!(fixture.run_dir.join("host-preflight.json").is_file());
    assert!(fixture.run_dir.join("host-postflight.json").is_file());
    assert!(fixture.run_dir.join("session/evidence").is_dir());
    assert!(fixture.run_dir.join("analysis-bundle").is_dir());
}

#[test]
fn runner_encloses_retained_failure_archive_before_returning_batch_status() {
    let fixture = Fixture::new();
    let output = fixture.run("1");
    assert_eq!(output.status.code(), Some(1));
    assert_eq!(fixture.command_names(), expected_command_names());
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(
        stderr.contains("retained failure evidence was enclosed and verified"),
        "unexpected stderr:\n{stderr}"
    );
    assert!(fixture.run_dir.join("host-postflight.json").is_file());
    assert!(fixture.run_dir.join("session/evidence").is_dir());
    assert!(fixture.run_dir.join("analysis-bundle").is_dir());
}

fn expected_command_names() -> Vec<String> {
    vec![
        "db-lab-host-preflight",
        "db-lab-batch",
        "db-lab-host-preflight",
        "db-lab-batch-verify",
        "db-lab-publication-session:create",
        "db-lab-publication-session:verify",
        "db-lab-batch-analysis-bundle:create",
        "db-lab-batch-analysis-bundle:verify",
    ]
    .into_iter()
    .map(str::to_owned)
    .collect()
}

struct Fixture {
    _root: tempfile::TempDir,
    bin_dir: PathBuf,
    trace: PathBuf,
    run_dir: PathBuf,
    log: PathBuf,
}

impl Fixture {
    fn new() -> Self {
        let root = tempdir().expect("temporary root");
        let bin_dir = root.path().join("bin");
        fs::create_dir(&bin_dir).expect("create fake bin directory");
        let log = root.path().join("commands.log");
        let trace = root.path().join("trace.json");
        fs::write(&trace, b"{}\n").expect("write fake trace");
        let run_dir = root.path().join("run");

        write_executable(&bin_dir.join("uname"), FAKE_UNAME);
        write_executable(&bin_dir.join("taskset"), FAKE_TASKSET);
        for name in REQUIRED_BINS {
            write_executable(&bin_dir.join(name), FAKE_DB_LAB);
        }

        Self {
            _root: root,
            bin_dir,
            trace,
            run_dir,
            log,
        }
    }

    fn run(&self, batch_status: &str) -> Output {
        let script = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../..")
            .join("scripts/run-controlled-publication.sh");
        let path = format!(
            "{}:{}",
            self.bin_dir.display(),
            std::env::var("PATH").expect("PATH")
        );
        Command::new("bash")
            .arg(script)
            .args([
                "--bin-dir",
                self.bin_dir.to_str().expect("bin path"),
                "--trace",
                self.trace.to_str().expect("trace path"),
                "--run-dir",
                self.run_dir.to_str().expect("run path"),
                "--revision",
                "0123456789abcdef0123456789abcdef01234567",
                "--pair-seed",
                "42",
                "--pairs",
                "4",
                "--expected-cpus",
                "2-3",
                "--max-load-per-cpu",
                "0.5",
                "--host-label",
                "fake-host",
                "--host-cpu",
                "fake-cpu",
                "--host-memory",
                "fake-memory",
                "--filesystem",
                "fakefs",
                "--mount-options",
                "rw",
                "--storage-device",
                "fake-device",
                "--thermal-attestation",
                "stable fake thermal state",
                "--background-attestation",
                "no fake background work",
                "--storage-cache-attestation",
                "fake warm cache policy",
                "--optimization-flags",
                "release fake flags",
                "--analysis-script-version",
                "fake-analysis-v1",
                "--noise-budget",
                "fake-noise-v1",
            ])
            .env("PATH", path)
            .env("FAKE_LOG", &self.log)
            .env("FAKE_BATCH_STATUS", batch_status)
            .output()
            .expect("run controlled publication script")
    }

    fn command_names(&self) -> Vec<String> {
        fs::read_to_string(&self.log)
            .expect("read command log")
            .lines()
            .map(str::to_owned)
            .collect()
    }
}

fn write_executable(path: &Path, contents: &str) {
    fs::write(path, contents).expect("write fake executable");
    let mut permissions = fs::metadata(path)
        .expect("fake executable metadata")
        .permissions();
    permissions.set_mode(0o755);
    fs::set_permissions(path, permissions).expect("chmod fake executable");
}

const FAKE_UNAME: &str = r#"#!/usr/bin/env bash
printf 'Linux\n'
"#;

const FAKE_TASKSET: &str = r#"#!/usr/bin/env bash
set -euo pipefail
[[ ${1:-} == -c ]] || exit 91
shift 2
exec "$@"
"#;

const FAKE_DB_LAB: &str = r#"#!/usr/bin/env bash
set -euo pipefail
name=$(basename "$0")
command_name=$name
if [[ $name == db-lab-publication-session || $name == db-lab-batch-analysis-bundle ]]; then
  command_name="$name:${1:-}"
fi
printf '%s\n' "$command_name" >> "$FAKE_LOG"

value_after() {
  local wanted=$1
  shift
  while [[ $# -gt 0 ]]; do
    if [[ $1 == "$wanted" ]]; then
      [[ $# -ge 2 ]] || exit 92
      printf '%s' "$2"
      return 0
    fi
    shift
  done
  exit 93
}

case "$name" in
  db-lab-host-preflight)
    output=$(value_after --output "$@")
    printf '{"passed":true}\n' > "$output"
    ;;
  db-lab-batch)
    archive=$(value_after --archive-dir "$@")
    mkdir "$archive"
    printf '{}\n' > "$archive/index.json"
    exit "${FAKE_BATCH_STATUS:-0}"
    ;;
  db-lab-batch-verify)
    archive=$(value_after --archive-dir "$@")
    [[ -d $archive ]]
    ;;
  db-lab-publication-session)
    subcommand=${1:-}
    shift
    if [[ $subcommand == create ]]; then
      session=$(value_after --session-dir "$@")
      archive=$(value_after --archive-dir "$@")
      mkdir -p "$session/evidence"
      cp "$archive/index.json" "$session/evidence/index.json"
    elif [[ $subcommand == verify ]]; then
      session=$(value_after --session-dir "$@")
      [[ -d $session/evidence ]]
    else
      exit 94
    fi
    ;;
  db-lab-batch-analysis-bundle)
    subcommand=${1:-}
    shift
    if [[ $subcommand == create ]]; then
      archive=$(value_after --archive-dir "$@")
      bundle=$(value_after --bundle-dir "$@")
      [[ -d $archive ]]
      mkdir "$bundle"
    elif [[ $subcommand == verify ]]; then
      bundle=$(value_after --bundle-dir "$@")
      [[ -d $bundle ]]
    else
      exit 95
    fi
    ;;
  *) exit 96 ;;
esac
"#;
