use std::collections::HashMap;

use serde::{Deserialize, Serialize};

const MAX_SOURCE_BYTES: usize = 2 * 1024 * 1024;
const MAX_OUTPUT_LINES: usize = 1_000;

#[derive(Clone, Debug, Default, Deserialize)]
#[serde(default, rename_all = "camelCase")]
pub struct DiffOptions {
    pub ignore_case: bool,
    pub ignore_whitespace: bool,
    pub ignore_comments: bool,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ProgramDiffParams {
    #[serde(default)]
    pub base: String,
    #[serde(default)]
    pub database: String,
    #[serde(default)]
    pub local: String,
    #[serde(default)]
    pub options: DiffOptions,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ProgramDiffResult {
    pub unified: String,
    pub merged: String,
    pub conflicts: usize,
    pub truncated: bool,
}

#[derive(Clone, Debug, PartialEq, Eq)]
enum Operation {
    Equal(String),
    Delete(String),
    Insert(String),
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct Change {
    start: usize,
    end: usize,
    replacement: Vec<String>,
}

pub fn compare(params: &ProgramDiffParams) -> ProgramDiffResult {
    let truncated = params.base.len() > MAX_SOURCE_BYTES
        || params.database.len() > MAX_SOURCE_BYTES
        || params.local.len() > MAX_SOURCE_BYTES;
    let base = bounded_lines(&params.base);
    let database = bounded_lines(&params.database);
    let local = bounded_lines(&params.local);
    let operations = patience_diff(&database, &local, &params.options);
    let unified = unified_text(&operations, truncated);
    let (merged, conflicts) = three_way_merge(&base, &database, &local, &params.options);
    ProgramDiffResult {
        unified,
        merged: merged.join("\n"),
        conflicts,
        truncated,
    }
}

fn bounded_lines(source: &str) -> Vec<String> {
    let end = source
        .char_indices()
        .take_while(|(index, _)| *index <= MAX_SOURCE_BYTES)
        .last()
        .map_or(0, |(index, character)| index + character.len_utf8())
        .min(source.len());
    source[..end]
        .replace("\r\n", "\n")
        .split('\n')
        .map(ToOwned::to_owned)
        .collect()
}

fn normalized_lines(lines: &[String], options: &DiffOptions) -> Vec<String> {
    let mut block_comment = false;
    lines
        .iter()
        .map(|line| normalize_line(line, options, &mut block_comment))
        .collect()
}

fn normalize_line(line: &str, options: &DiffOptions, block_comment: &mut bool) -> String {
    let mut value = if options.ignore_comments {
        strip_comments(line, block_comment)
    } else {
        line.to_owned()
    };
    if options.ignore_whitespace {
        value = value.split_whitespace().collect::<Vec<_>>().join(" ");
    }
    if options.ignore_case {
        value.make_ascii_uppercase();
    }
    value
}

fn strip_comments(line: &str, block_comment: &mut bool) -> String {
    let bytes = line.as_bytes();
    let mut output = String::new();
    let mut index = 0;
    let mut quote = None;
    while index < bytes.len() {
        let current = bytes[index] as char;
        let next = bytes.get(index + 1).copied().map(char::from);
        if *block_comment {
            if current == '*' && next == Some('/') {
                *block_comment = false;
                index += 2;
            } else {
                index += 1;
            }
            continue;
        }
        if quote.is_none() && current == '-' && next == Some('-') {
            break;
        }
        if quote.is_none() && current == '/' && next == Some('*') {
            *block_comment = true;
            index += 2;
            continue;
        }
        if current == '\'' || current == '"' {
            if quote == Some(current) && next == Some(current) {
                output.push(current);
                output.push(current);
                index += 2;
                continue;
            }
            if quote == Some(current) {
                quote = None;
            } else if quote.is_none() {
                quote = Some(current);
            }
        }
        output.push(current);
        index += 1;
    }
    output
}

fn patience_diff(left: &[String], right: &[String], options: &DiffOptions) -> Vec<Operation> {
    let left_keys = normalized_lines(left, options);
    let right_keys = normalized_lines(right, options);
    let mut output = Vec::new();
    diff_range(
        left,
        right,
        &left_keys,
        &right_keys,
        0,
        left.len(),
        0,
        right.len(),
        &mut output,
    );
    output
}

#[allow(clippy::too_many_arguments)]
fn diff_range(
    left: &[String],
    right: &[String],
    left_keys: &[String],
    right_keys: &[String],
    left_start: usize,
    left_end: usize,
    right_start: usize,
    right_end: usize,
    output: &mut Vec<Operation>,
) {
    let mut prefix = 0;
    while left_start + prefix < left_end
        && right_start + prefix < right_end
        && left_keys[left_start + prefix] == right_keys[right_start + prefix]
    {
        output.push(Operation::Equal(left[left_start + prefix].clone()));
        prefix += 1;
    }
    let left_start = left_start + prefix;
    let right_start = right_start + prefix;
    let mut suffix = 0;
    while left_end > left_start + suffix
        && right_end > right_start + suffix
        && left_keys[left_end - suffix - 1] == right_keys[right_end - suffix - 1]
    {
        suffix += 1;
    }
    let left_middle_end = left_end - suffix;
    let right_middle_end = right_end - suffix;
    if left_start == left_middle_end {
        for value in &right[right_start..right_middle_end] {
            output.push(Operation::Insert(value.clone()));
        }
    } else if right_start == right_middle_end {
        for value in &left[left_start..left_middle_end] {
            output.push(Operation::Delete(value.clone()));
        }
    } else if let Some(anchors) = unique_anchors(
        &left_keys[left_start..left_middle_end],
        &right_keys[right_start..right_middle_end],
    ) {
        let mut previous_left = left_start;
        let mut previous_right = right_start;
        for (left_offset, right_offset) in anchors {
            let left_anchor = left_start + left_offset;
            let right_anchor = right_start + right_offset;
            diff_range(
                left,
                right,
                left_keys,
                right_keys,
                previous_left,
                left_anchor,
                previous_right,
                right_anchor,
                output,
            );
            output.push(Operation::Equal(left[left_anchor].clone()));
            previous_left = left_anchor + 1;
            previous_right = right_anchor + 1;
        }
        diff_range(
            left,
            right,
            left_keys,
            right_keys,
            previous_left,
            left_middle_end,
            previous_right,
            right_middle_end,
            output,
        );
    } else {
        for value in &left[left_start..left_middle_end] {
            output.push(Operation::Delete(value.clone()));
        }
        for value in &right[right_start..right_middle_end] {
            output.push(Operation::Insert(value.clone()));
        }
    }
    for value in &left[left_middle_end..left_end] {
        output.push(Operation::Equal(value.clone()));
    }
}

fn unique_anchors(left: &[String], right: &[String]) -> Option<Vec<(usize, usize)>> {
    let mut left_positions: HashMap<&str, Vec<usize>> = HashMap::new();
    let mut right_positions: HashMap<&str, Vec<usize>> = HashMap::new();
    for (index, value) in left.iter().enumerate() {
        left_positions.entry(value).or_default().push(index);
    }
    for (index, value) in right.iter().enumerate() {
        right_positions.entry(value).or_default().push(index);
    }
    let candidates = left_positions
        .into_iter()
        .filter_map(|(value, positions)| {
            let right_match = right_positions.get(value)?;
            (positions.len() == 1 && right_match.len() == 1)
                .then_some((positions[0], right_match[0]))
        })
        .collect::<Vec<_>>();
    if candidates.is_empty() {
        return None;
    }
    let mut sorted = candidates;
    sorted.sort_unstable_by_key(|(left_index, _)| *left_index);
    let mut tails: Vec<usize> = Vec::new();
    let mut previous = vec![None; sorted.len()];
    for index in 0..sorted.len() {
        let position = tails
            .binary_search_by_key(&sorted[index].1, |candidate| sorted[*candidate].1)
            .unwrap_or_else(|value| value);
        if position > 0 {
            previous[index] = Some(tails[position - 1]);
        }
        if position == tails.len() {
            tails.push(index);
        } else {
            tails[position] = index;
        }
    }
    let mut chain = Vec::new();
    let mut current = tails.last().copied();
    while let Some(index) = current {
        chain.push(sorted[index]);
        current = previous[index];
    }
    chain.reverse();
    Some(chain)
}

fn unified_text(operations: &[Operation], truncated: bool) -> String {
    let mut output = vec!["--- 数据库源码".to_owned(), "+++ 本地源码".to_owned()];
    let mut emitted = 0;
    let mut index = 0;
    while index < operations.len() && emitted < MAX_OUTPUT_LINES {
        if matches!(operations[index], Operation::Equal(_)) {
            index += 1;
            continue;
        }
        let start = index;
        while index < operations.len() && !matches!(operations[index], Operation::Equal(_)) {
            index += 1;
        }
        output.push(format!("@@ 差异块 {} @@", output.len() - 1));
        for operation in &operations[start..index] {
            if emitted >= MAX_OUTPUT_LINES {
                break;
            }
            match operation {
                Operation::Delete(value) => output.push(format!("- {value}")),
                Operation::Insert(value) => output.push(format!("+ {value}")),
                Operation::Equal(_) => {}
            }
            emitted += 1;
        }
    }
    if emitted == 0 {
        output.push("没有差异。".to_owned());
    }
    if index < operations.len() {
        output.push(format!("…差异过大，仅显示前 {MAX_OUTPUT_LINES} 行。"));
    }
    if truncated {
        output.push("…源码超过 200 万字节，差异比较已按上限截断。".to_owned());
    }
    output.join("\n")
}

fn changes(base: &[String], other: &[String], options: &DiffOptions) -> Vec<Change> {
    let operations = patience_diff(base, other, options);
    let mut output = Vec::new();
    let mut base_index = 0;
    let mut current: Option<Change> = None;
    for operation in operations {
        match operation {
            Operation::Equal(_) => {
                if let Some(change) = current.take() {
                    output.push(change);
                }
                base_index += 1;
            }
            Operation::Delete(_) => {
                let change = current.get_or_insert(Change {
                    start: base_index,
                    end: base_index,
                    replacement: Vec::new(),
                });
                base_index += 1;
                change.end = base_index;
            }
            Operation::Insert(value) => {
                current
                    .get_or_insert(Change {
                        start: base_index,
                        end: base_index,
                        replacement: Vec::new(),
                    })
                    .replacement
                    .push(value);
            }
        }
    }
    if let Some(change) = current {
        output.push(change);
    }
    output
}

fn three_way_merge(
    base: &[String],
    database: &[String],
    local: &[String],
    options: &DiffOptions,
) -> (Vec<String>, usize) {
    if database == local {
        return (database.to_vec(), 0);
    }
    if base == database {
        return (local.to_vec(), 0);
    }
    if base == local {
        return (database.to_vec(), 0);
    }
    let remote = changes(base, database, options);
    let local_changes = changes(base, local, options);
    let mut merged = Vec::new();
    let mut base_index = 0;
    let mut remote_index = 0;
    let mut local_index = 0;
    let mut conflicts = 0;
    while remote_index < remote.len() || local_index < local_changes.len() {
        let remote_change = remote.get(remote_index);
        let local_change = local_changes.get(local_index);
        let next_start = remote_change
            .map(|value| value.start)
            .into_iter()
            .chain(local_change.map(|value| value.start))
            .min()
            .unwrap_or(base.len());
        merged.extend_from_slice(&base[base_index..next_start.min(base.len())]);
        match (remote_change, local_change) {
            (Some(left), Some(right)) if left.start <= right.end && right.start <= left.end => {
                let end = left.end.max(right.end);
                if left.start == right.start
                    && left.end == right.end
                    && left.replacement == right.replacement
                {
                    merged.extend(left.replacement.clone());
                } else {
                    conflicts += 1;
                    merged.push("<<<<<<< DATABASE".to_owned());
                    merged.extend(left.replacement.clone());
                    merged.push("=======".to_owned());
                    merged.extend(right.replacement.clone());
                    merged.push(">>>>>>> LOCAL".to_owned());
                }
                base_index = end;
                remote_index += 1;
                local_index += 1;
            }
            (Some(left), _) if local_change.is_none_or(|right| left.start < right.start) => {
                merged.extend(left.replacement.clone());
                base_index = left.end;
                remote_index += 1;
            }
            (_, Some(right)) => {
                merged.extend(right.replacement.clone());
                base_index = right.end;
                local_index += 1;
            }
            _ => break,
        }
    }
    merged.extend_from_slice(&base[base_index.min(base.len())..]);
    (merged, conflicts)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reports_multiple_hunks_and_merges_non_overlapping_changes() {
        let result = compare(&ProgramDiffParams {
            base: "a\nb\nc\nd".to_owned(),
            database: "a\nserver\nc\nd".to_owned(),
            local: "a\nb\nc\nlocal".to_owned(),
            options: DiffOptions::default(),
        });
        assert!(result.unified.contains("server"));
        assert!(result.unified.contains("local"));
        assert_eq!(result.conflicts, 0);
        assert_eq!(result.merged, "a\nserver\nc\nlocal");
    }

    #[test]
    fn marks_overlapping_three_way_conflicts() {
        let result = compare(&ProgramDiffParams {
            base: "a\nb\nc".to_owned(),
            database: "a\nserver\nc".to_owned(),
            local: "a\nlocal\nc".to_owned(),
            options: DiffOptions::default(),
        });
        assert_eq!(result.conflicts, 1);
        assert!(result.merged.contains("<<<<<<< DATABASE"));
    }

    #[test]
    fn ignore_options_affect_matching_without_rewriting_sources() {
        let result = compare(&ProgramDiffParams {
            base: String::new(),
            database: "select  1; -- database".to_owned(),
            local: "SELECT 1; -- local".to_owned(),
            options: DiffOptions {
                ignore_case: true,
                ignore_whitespace: true,
                ignore_comments: true,
            },
        });
        assert!(result.unified.contains("没有差异"));
    }
}
