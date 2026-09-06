use std::path::Path;

use db_cli::generation_directory::parse_canonical_generation_name;
use db_storage_log::is_canonical_generation_path;

#[test]
fn cli_and_storage_agree_on_canonical_generation_path_policy() {
    let cases = [
        ("generation-00000000000000000001.log", true),
        ("generation-00000000000000000042.log", true),
        ("generation-18446744073709551615.log", true),
        ("generation-00000000000000000000.log", false),
        ("generation-18446744073709551616.log", false),
        ("generation-1.log", false),
        ("generation-0000000000000000001.log", false),
        ("generation-000000000000000000001.log", false),
        ("generation-00000000000000000001", false),
        ("generation-00000000000000000001.log.extra", false),
        ("x-generation-00000000000000000001.log", false),
        ("GENERATION-00000000000000000001.log", false),
        (
            "generation-０００００００００００００００００００１.log",
            false,
        ),
        ("ordinary-raw.log", false),
        ("", false),
    ];

    for (name, expected) in cases {
        let cli_canonical = matches!(parse_canonical_generation_name(name), Ok(Some(_)));
        let storage_canonical = is_canonical_generation_path(Path::new(name));
        assert_eq!(
            cli_canonical, expected,
            "unexpected CLI policy for {name:?}"
        );
        assert_eq!(
            storage_canonical, expected,
            "unexpected storage policy for {name:?}"
        );
        assert_eq!(
            cli_canonical, storage_canonical,
            "CLI/storage canonical-generation policy drifted for {name:?}"
        );
    }
}

#[cfg(unix)]
#[test]
fn storage_predicate_rejects_non_utf8_generation_like_filename() {
    use std::ffi::OsString;
    use std::os::unix::ffi::OsStringExt;

    let mut bytes = b"generation-00000000000000000001.log".to_vec();
    bytes[11] = 0xff;
    let path = std::path::PathBuf::from(OsString::from_vec(bytes));
    assert!(!is_canonical_generation_path(path));
}
