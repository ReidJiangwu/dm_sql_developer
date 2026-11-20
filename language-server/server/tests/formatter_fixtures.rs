use std::{fs, path::PathBuf};

use reid_dm_language_server::formatter::{FormatterProfile, Preset, format_sql};

#[test]
fn shared_formatter_cases_match_expected_output() {
    let fixture_path = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../shared/test-fixtures/sql-formatter-cases.tsv");
    let fixture = fs::read_to_string(&fixture_path).expect("shared formatter fixture must exist");

    for (line_number, line) in fixture.lines().enumerate().skip(1) {
        if line.trim().is_empty() {
            continue;
        }
        let fields = line.split('\t').collect::<Vec<_>>();
        assert_eq!(
            fields.len(),
            4,
            "invalid formatter fixture row at {}",
            line_number + 1
        );
        let id = fields[0];
        let mut profile = FormatterProfile::default();
        profile.preset = match fields[1] {
            "standard" => Preset::Standard,
            "compact" => Preset::Compact,
            "expanded" => Preset::Expanded,
            other => panic!("{id} has unsupported profile {other}"),
        };
        let actual = format_sql(&unescape(fields[2]), profile, false, None)
            .unwrap_or_else(|error| panic!("{id} formatting failed: {error}"));
        assert_eq!(actual, unescape(fields[3]), "{id} output");
    }
}

fn unescape(value: &str) -> String {
    value
        .replace("\\r", "\r")
        .replace("\\n", "\n")
        .replace("\\t", "\t")
}
