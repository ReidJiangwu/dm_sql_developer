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
    semantic: std::sync::OnceLock<SemanticSnapshot>,
    parser: Parser,
}

impl Document {
    pub fn open(version: i64, text: String, connection_id: Option<String>) -> Result<Self, String> {
        let mut parser = parser()?;
        let rope = Rope::from_str(&text);
        let tree = parse_if_supported(&mut parser, &text, None);
        Ok(Self {
            version,
            connection_id,
            text: rope,
            tree,
            semantic: Default::default(),
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
        if changes.is_empty() {
            self.version = version;
            return Ok(());
        }
        // Ranges in a batch are relative to the preceding edit. Restore the
        // original revision if a later range is invalid.
        let original = (changes.len() > 1).then(|| (self.text.clone(), self.tree.clone()));
        for change in changes {
            match change.range {
                Some(range) => {
                    if let Err(error) = self.apply_incremental_change(range, &change.text) {
                        if let Some((text, tree)) = original {
                            self.text = text;
                            self.tree = tree;
                        }
                        return Err(error);
                    }
                }
                None => {
                    self.text = Rope::from_str(&change.text);
                    self.tree = None;
                }
            }
        }
        self.version = version;
        let text = &self.text;
        self.tree = if text.len_bytes() <= MAX_DOCUMENT_BYTES {
            self.parser.parse_with_options(&mut |byte, _| {
                if byte >= text.len_bytes() {
                    return &[][..];
                }
                let (chunk, start, _, _) = text.chunk_at_byte(byte);
                &chunk.as_bytes()[byte - start..]
            }, self.tree.as_ref(), None)
        } else {
            None
        };
        self.semantic.take();
        Ok(())
    }

    pub fn semantic(&self) -> &SemanticSnapshot {
        self.semantic.get_or_init(|| SemanticSnapshot::build(&self.source(), self.has_syntax_errors()))
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

    /// Returns whether a new text revision needs diagnostics.
    pub fn change(&mut self, params: DidChangeParams) -> Result<bool, String> {
        let Some(document) = self.documents.get_mut(&params.text_document.uri) else {
            return Err("document is not open".to_owned());
        };
        let version = params.text_document.version;
        if version <= document.version {
            return Ok(false);
        }
        let text_changed = !params.content_changes.is_empty();
        document.apply_changes(version, params.content_changes)?;
        if params.connection_id.is_some() {
            document.connection_id = params.connection_id;
        }
        Ok(text_changed)
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
    const LIMIT: usize = 100;
    if !node.has_error() && !node.is_missing() {
        return;
    }
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
            if ranges.len() >= LIMIT {
                break;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn connection_change_preserves_text_caches_and_rejects_late_replies() {
        let mut store = DocumentStore::default();
        store.open(DidOpenParams {
            text_document: TextDocumentItem { uri: "doc".into(), version: 1, text: "SELECT 1;".into() },
            connection_id: Some("first".into()),
        }).unwrap();
        let doc = store.get("doc").unwrap();
        let node = doc.tree.as_ref().unwrap().root_node().id();
        doc.semantic();
        let switch = |version, id: &str| DidChangeParams {
            text_document: VersionedTextDocumentIdentifier { uri: "doc".into(), version },
            content_changes: vec![], connection_id: Some(id.into()),
        };
        assert!(!store.change(switch(3, "second")).unwrap());
        assert!(!store.change(switch(2, "stale")).unwrap());
        let doc = store.get("doc").unwrap();
        assert_eq!(doc.version, 3);
        assert_eq!(doc.connection_id.as_deref(), Some("second"));
        assert!(doc.semantic.get().is_some());
        assert_eq!(doc.tree.as_ref().unwrap().root_node().id(), node);
    }

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
    fn incremental_parse_across_rope_chunks_matches_a_fresh_parse() {
        let sql = format!("/* {} */\nSELECT old_name FROM t;", "跨块🙂".repeat(2000));
        let mut doc = Document::open(1, sql.clone(), None).unwrap();
        let range = Range {
            start: Position { line: 1, character: 7 },
            end: Position { line: 1, character: 15 },
        };
        let replacement = "\"新🙂列\", second_col";
        doc.apply_changes(2, vec![ContentChange {
            range: Some(range), text: replacement.into(),
        }]).unwrap();
        let expected = sql.replace("old_name", replacement);
        let fresh = Document::open(1, expected.clone(), None).unwrap();
        let root = doc.tree.as_ref().unwrap().root_node();
        assert_eq!(root.end_byte(), expected.len());
        assert_eq!(root.to_sexp(), fresh.tree.as_ref().unwrap().root_node().to_sexp());
    }

    #[test]
    fn semantic_snapshot_follows_the_committed_version() {
        let mut doc = Document::open(1, "SELECT a.x FROM t a".into(), None).unwrap();
        assert!(doc.semantic.get().is_none());
        let _ = doc.semantic();
        assert!(doc.semantic.get().is_some());
        doc.apply_changes(2, vec![ContentChange {
            range: None, text: "SELECT b.x FROM t b".into(),
        }]).unwrap();
        assert!(doc.semantic.get().is_none());
        assert!(doc.semantic().resolve_alias("a", 8).is_none());
        let failed = doc.apply_changes(3, vec![
            ContentChange { range: None, text: "SELECT 1".into() },
            ContentChange { range: Some(Range {
                start: Position { line: 9, character: 0 },
                end: Position { line: 9, character: 1 },
            }), text: String::new() },
        ]);
        assert!(failed.is_err());
        assert_eq!(doc.version, 2);
        assert_eq!(doc.source(), "SELECT b.x FROM t b");
        assert!(doc.semantic.get().is_some());
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

    #[test]
    fn unicode_source_open_change_and_symbols_do_not_panic() {
        let source = "CREATE OR REPLACE PACKAGE p AS \u{a0}field_ NUMBER; END p;";
        let mut document = Document::open(1, source.into(), None).unwrap();
        let symbols = crate::symbols::document_symbols(&document);
        assert!(serde_json::to_string(&symbols).unwrap().contains("field_"));
        document.apply_changes(2, vec![ContentChange {
            range: None,
            text: "CREATE OR REPLACE PACKAGE p AS 中文 NUMBER; END p;".into(),
        }]).unwrap();
        let _ = document.syntax_error_ranges();
        let _ = crate::symbols::document_symbols(&document);
        document.apply_changes(3, vec![ContentChange {
            range: Some(Range {
                start: Position { line: 0, character: 31 },
                end: Position { line: 0, character: 33 },
            }),
            text: "restored_".into(),
        }]).unwrap();
        assert!(document.source().contains("restored_ NUMBER"));
        assert!(serde_json::to_string(&crate::symbols::document_symbols(&document))
            .unwrap().contains("restored_"));
    }
}
