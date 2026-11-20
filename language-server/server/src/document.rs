use std::collections::HashMap;

use ropey::Rope;
use serde::{Deserialize, Serialize};
use tree_sitter::{InputEdit, Parser, Point, Tree};

use crate::semantic::SemanticSnapshot;

pub const MAX_DOCUMENT_BYTES: usize = 8 * 1024 * 1024;

#[derive(Clone, Copy, Debug, Default, Deserialize, Serialize)]
pub struct Position {
    pub line: u32,
    pub character: u32,
}

#[derive(Clone, Copy, Debug, Default, Deserialize, Serialize)]
pub struct Range {
    pub start: Position,
    pub end: Position,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct TextDocumentItem {
    pub uri: String,
    pub version: i64,
    pub text: String,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct VersionedTextDocumentIdentifier {
    pub uri: String,
    pub version: i64,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct TextDocumentIdentifier {
    pub uri: String,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DidOpenParams {
    pub text_document: TextDocumentItem,
    #[serde(default)]
    pub connection_id: Option<String>,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ContentChange {
    #[serde(default)]
    pub range: Option<Range>,
    pub text: String,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DidChangeParams {
    pub text_document: VersionedTextDocumentIdentifier,
    pub content_changes: Vec<ContentChange>,
    #[serde(default)]
    pub connection_id: Option<String>,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DidCloseParams {
    pub text_document: TextDocumentIdentifier,
}

pub struct Document {
    pub version: i64,
    pub connection_id: Option<String>,
    pub text: Rope,
    pub tree: Option<Tree>,
    pub semantic: SemanticSnapshot,
    parser: Parser,
}

impl Document {
    pub fn open(version: i64, text: String, connection_id: Option<String>) -> Result<Self, String> {
        let mut parser = parser()?;
        let rope = Rope::from_str(&text);
        let tree = parse_if_supported(&mut parser, &text, None);
        let has_errors = tree
            .as_ref()
            .is_some_and(|tree| tree.root_node().has_error());
        let semantic = SemanticSnapshot::build(&text, has_errors);
        Ok(Self {
            version,
            connection_id,
            text: rope,
            tree,
            semantic,
            parser,
        })
    }

    pub fn apply_changes(
        &mut self,
        version: i64,
        changes: Vec<ContentChange>,
    ) -> Result<(), String> {
        if version <= self.version {
            return Ok(());
        }
        for change in changes {
            match change.range {
                Some(range) => self.apply_incremental_change(range, &change.text)?,
                None => {
                    self.text = Rope::from_str(&change.text);
                    self.tree = parse_if_supported(&mut self.parser, &change.text, None);
                }
            }
        }
        self.version = version;
        let text = self.text.to_string();
        let has_errors = self
            .tree
            .as_ref()
            .is_some_and(|tree| tree.root_node().has_error());
        self.semantic = SemanticSnapshot::build(&text, has_errors);
        Ok(())
    }

    pub fn byte_offset(&self, position: Position) -> usize {
        position_to_char(&self.text, position)
            .map(|char_index| self.text.char_to_byte(char_index))
            .unwrap_or_else(|| self.text.len_bytes())
    }

    pub fn source(&self) -> String {
        self.text.to_string()
    }

    pub fn position_at_byte(&self, byte: usize) -> Position {
        let byte = byte.min(self.text.len_bytes());
        let char_index = self.text.byte_to_char(byte);
        let line = self.text.char_to_line(char_index);
        let line_start = self.text.line_to_char(line);
        let utf16 = self
            .text
            .slice(line_start..char_index)
            .chars()
            .map(char::len_utf16)
            .sum::<usize>();
        Position {
            line: u32::try_from(line).unwrap_or(u32::MAX),
            character: u32::try_from(utf16).unwrap_or(u32::MAX),
        }
    }

    pub fn syntax_error_ranges(&self) -> Vec<Range> {
        let Some(tree) = &self.tree else {
            return Vec::new();
        };
        let mut byte_ranges = Vec::new();
        collect_error_ranges(tree.root_node(), &mut byte_ranges);
        byte_ranges
            .into_iter()
            .take(100)
            .map(|(start, end)| Range {
                start: self.position_at_byte(start),
                end: self.position_at_byte(end.max(start)),
            })
            .collect()
    }

    pub fn has_syntax_errors(&self) -> bool {
        self.tree
            .as_ref()
            .is_some_and(|tree| tree.root_node().has_error())
    }

    fn apply_incremental_change(&mut self, range: Range, inserted: &str) -> Result<(), String> {
        let start_char = position_to_char(&self.text, range.start)
            .ok_or_else(|| "incremental change start is outside document".to_owned())?;
        let old_end_char = position_to_char(&self.text, range.end)
            .ok_or_else(|| "incremental change end is outside document".to_owned())?;
        if old_end_char < start_char {
            return Err("incremental change range is reversed".to_owned());
        }

        let start_byte = self.text.char_to_byte(start_char);
        let old_end_byte = self.text.char_to_byte(old_end_char);
        let start_position = point_at_char(&self.text, start_char);
        let old_end_position = point_at_char(&self.text, old_end_char);
        self.text.remove(start_char..old_end_char);
        self.text.insert(start_char, inserted);
        let new_end_byte = start_byte + inserted.len();
        let new_end_position = point_after_insert(start_position, inserted);

        if let Some(tree) = self.tree.as_mut() {
            tree.edit(&InputEdit {
                start_byte,
                old_end_byte,
                new_end_byte,
                start_position,
                old_end_position,
                new_end_position,
            });
        }
        let text = self.text.to_string();
        self.tree = parse_if_supported(&mut self.parser, &text, self.tree.as_ref());
        Ok(())
    }
}

#[derive(Default)]
pub struct DocumentStore {
    documents: HashMap<String, Document>,
}

impl DocumentStore {
    pub fn open(&mut self, params: DidOpenParams) -> Result<(), String> {
        let item = params.text_document;
        self.documents.insert(
            item.uri,
            Document::open(item.version, item.text, params.connection_id)?,
        );
        Ok(())
    }

    pub fn change(&mut self, params: DidChangeParams) -> Result<(), String> {
        let Some(document) = self.documents.get_mut(&params.text_document.uri) else {
            return Err("document is not open".to_owned());
        };
        if params.connection_id.is_some() {
            document.connection_id = params.connection_id;
        }
        document.apply_changes(params.text_document.version, params.content_changes)
    }

    pub fn close(&mut self, params: DidCloseParams) {
        self.documents.remove(&params.text_document.uri);
    }

    pub fn get(&self, uri: &str) -> Option<&Document> {
        self.documents.get(uri)
    }
}

fn parser() -> Result<Parser, String> {
    let mut parser = Parser::new();
    parser
        .set_language(&tree_sitter_dm_sql::LANGUAGE.into())
        .map_err(|error| format!("cannot load DM SQL grammar: {error}"))?;
    Ok(parser)
}

fn parse_if_supported(parser: &mut Parser, text: &str, old_tree: Option<&Tree>) -> Option<Tree> {
    (text.len() <= MAX_DOCUMENT_BYTES)
        .then(|| parser.parse(text, old_tree))
        .flatten()
}

fn position_to_char(rope: &Rope, position: Position) -> Option<usize> {
    let line = usize::try_from(position.line).ok()?;
    if line >= rope.len_lines() {
        return (line == rope.len_lines() && position.character == 0).then_some(rope.len_chars());
    }
    let line_start = rope.line_to_char(line);
    let line_slice = rope.line(line);
    let target_utf16 = usize::try_from(position.character).ok()?;
    let mut utf16 = 0_usize;
    let mut chars = 0_usize;
    for character in line_slice.chars() {
        if utf16 == target_utf16 {
            return Some(line_start + chars);
        }
        let width = character.len_utf16();
        if utf16 + width > target_utf16 {
            return None;
        }
        utf16 += width;
        chars += 1;
    }
    (utf16 == target_utf16).then_some(line_start + chars)
}

fn point_at_char(rope: &Rope, char_index: usize) -> Point {
    let line = rope.char_to_line(char_index);
    let line_start_char = rope.line_to_char(line);
    let line_start_byte = rope.char_to_byte(line_start_char);
    let byte = rope.char_to_byte(char_index);
    Point::new(line, byte - line_start_byte)
}

fn point_after_insert(start: Point, inserted: &str) -> Point {
    let newline_count = inserted.bytes().filter(|byte| *byte == b'\n').count();
    if newline_count == 0 {
        Point::new(start.row, start.column + inserted.len())
    } else {
        let last_line_bytes = inserted
            .rsplit_once('\n')
            .map(|(_, line)| line.len())
            .unwrap_or(0);
        Point::new(start.row + newline_count, last_line_bytes)
    }
}

fn collect_error_ranges(node: tree_sitter::Node<'_>, ranges: &mut Vec<(usize, usize)>) {
    if node.is_error() || node.is_missing() {
        ranges.push((node.start_byte(), node.end_byte()));
        return;
    }
    for index in 0..node.child_count() {
        let Ok(index) = u32::try_from(index) else {
            break;
        };
        if let Some(child) = node.child(index) {
            collect_error_ranges(child, ranges);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn utf16_positions_handle_surrogate_pairs() {
        let rope = Rope::from_str("A😀B\n");
        assert_eq!(
            position_to_char(
                &rope,
                Position {
                    line: 0,
                    character: 3
                }
            ),
            Some(2)
        );
        assert_eq!(
            position_to_char(
                &rope,
                Position {
                    line: 0,
                    character: 2
                }
            ),
            None
        );
    }

    #[test]
    fn applies_incremental_edits_and_reparses() {
        let mut document = Document::open(1, "SELECT o. FROM ORDERS o".into(), None).unwrap();
        document
            .apply_changes(
                2,
                vec![ContentChange {
                    range: Some(Range {
                        start: Position {
                            line: 0,
                            character: 9,
                        },
                        end: Position {
                            line: 0,
                            character: 9,
                        },
                    }),
                    text: "AMOUNT".into(),
                }],
            )
            .unwrap();
        assert_eq!(document.source(), "SELECT o.AMOUNT FROM ORDERS o");
        assert_eq!(document.version, 2);
    }

    #[test]
    fn accepts_keyword_prefixed_variables_in_anonymous_block() {
        let source = "DECLARE return_array_ Manual_Supplier_Invoice_Handling_SVC.\
Posting_Structure_Art; cursor_ SYS_REFCURSOR; BEGIN return_array_ := \
Manual_Supplier_Invoice_Handling_SVC.Do_Post_Invoices('TESTGZYD003', 59, \
'TESTS1', 'SI', '2026080501', TO_DATE('2026-08-07','YYYY-MM-DD'), 'AC', 'I', \
NULL, 2026, 'FALSE', '1', 'Preliminary', 'FALSE', NULL, unbound## => ''); \
OPEN cursor_ FOR SELECT * FROM TABLE(return_array_); NULL := cursor_; END;";
        let document = Document::open(1, source.into(), None).unwrap();

        assert!(!document.has_syntax_errors());
        assert!(document.syntax_error_ranges().is_empty());
    }
}
