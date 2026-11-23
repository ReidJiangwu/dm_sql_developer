use std::borrow::Cow;

use ropey::Rope;

const MAX_BACKWARD_BYTES: usize = 64 * 1024;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum CompletionContext {
    Keyword,
    Schema,
    Relation,
    Column,
    Member,
    Routine,
    LocalVariable,
    BindVariable,
    TypeName,
}

impl CompletionContext {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Keyword => "Keyword",
            Self::Schema => "Schema",
            Self::Relation => "Relation",
            Self::Column => "Column",
            Self::Member => "Member",
            Self::Routine => "Routine",
            Self::LocalVariable => "LocalVariable",
            Self::BindVariable => "BindVariable",
            Self::TypeName => "TypeName",
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CompletionProbe {
    pub context: CompletionContext,
    pub prefix: String,
    pub qualifier: Option<String>,
    pub replace_start: usize,
    pub suppressed: bool,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum TokenKind {
    Word,
    Number,
    Symbol,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct LexToken {
    pub kind: TokenKind,
    pub text: String,
    pub start: usize,
    pub end: usize,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum LexState {
    Code,
    String,
    QuotedIdentifier,
    LineComment,
    BlockComment,
}

pub(crate) fn analyze_rope(text: &Rope, cursor_byte: usize) -> CompletionProbe {
    let end_char = text.byte_to_char(cursor_byte.min(text.len_bytes()));
    let end_byte = text.char_to_byte(end_char);
    let start_char = text.byte_to_char(end_byte.saturating_sub(MAX_BACKWARD_BYTES));
    // Include the preceding character so a bind colon before the window is visible.
    let base_char = start_char.saturating_sub(1);
    let window: Cow<'_, str> = text.slice(base_char..end_char).into();
    let mut probe = analyze(&window, window.len());
    probe.replace_start += text.char_to_byte(base_char);
    probe
}

pub fn analyze(text: &str, cursor_byte: usize) -> CompletionProbe {
    let cursor = clamp_char_boundary(text, cursor_byte.min(text.len()));
    let start = clamp_char_boundary(text, cursor.saturating_sub(MAX_BACKWARD_BYTES));
    let lexed = lex_sql(&text[start..cursor], start);
    if lexed.state != LexState::Code {
        return CompletionProbe {
            context: CompletionContext::Keyword,
            prefix: String::new(),
            qualifier: None,
            replace_start: cursor,
            suppressed: true,
        };
    }

    let statement_start = lexed.tokens.iter()
        .rposition(|token| token.kind == TokenKind::Symbol && token.text == ";")
        .map_or(0, |index| index + 1);
    let tokens = &lexed.tokens[statement_start..];
    let mut prefix = String::new();
    let mut replace_start = cursor;
    let mut before_prefix = tokens.len();
    if let Some(last) = tokens.last()
        && last.kind == TokenKind::Word
        && last.end == cursor
    {
        prefix = last.text.clone();
        replace_start = last.start;
        before_prefix -= 1;
    }

    let qualifier = if before_prefix > 0
        && tokens[before_prefix - 1].kind == TokenKind::Symbol
        && tokens[before_prefix - 1].text == "."
    {
        tokens[..before_prefix - 1]
            .iter()
            .rev()
            .find(|token| token.kind == TokenKind::Word)
            .map(|token| token.text.clone())
    } else {
        None
    };

    if qualifier.is_some() {
        return CompletionProbe {
            context: CompletionContext::Member,
            prefix,
            qualifier,
            replace_start,
            suppressed: false,
        };
    }

    if replace_start > 0 && text.as_bytes().get(replace_start - 1) == Some(&b':') {
        return CompletionProbe {
            context: CompletionContext::BindVariable,
            prefix,
            qualifier: None,
            replace_start,
            suppressed: false,
        };
    }

    let words: Vec<&str> = tokens[..before_prefix]
        .iter()
        .rev()
        .filter(|token| token.kind == TokenKind::Word)
        .take(8)
        .map(|token| token.text.as_str())
        .collect();
    let nearest = words.first().map(|word| word.to_ascii_uppercase());
    let context = match nearest.as_deref() {
        Some("FROM" | "JOIN" | "INTO" | "UPDATE" | "USING") => CompletionContext::Relation,
        Some("PROCEDURE" | "FUNCTION" | "CALL" | "EXEC" | "EXECUTE") => CompletionContext::Routine,
        Some("AS" | "IS") if contains_program_header(&words) => CompletionContext::TypeName,
        Some("DECLARE") => CompletionContext::LocalVariable,
        Some("SELECT" | "WHERE" | "ON" | "SET" | "HAVING" | "BY") => CompletionContext::Column,
        _ if inside_declare_section(tokens) => CompletionContext::LocalVariable,
        _ if inside_program_block(tokens) => CompletionContext::LocalVariable,
        _ => CompletionContext::Keyword,
    };

    CompletionProbe {
        context,
        prefix,
        qualifier: None,
        replace_start,
        suppressed: false,
    }
}

fn contains_program_header(words: &[&str]) -> bool {
    words
        .iter()
        .any(|word| matches!(word.to_ascii_uppercase().as_str(), "PROCEDURE" | "FUNCTION"))
}

fn inside_declare_section(tokens: &[LexToken]) -> bool {
    let mut declare_depth = 0_u32;
    for token in tokens {
        if token.kind != TokenKind::Word {
            continue;
        }
        match token.text.to_ascii_uppercase().as_str() {
            "DECLARE" => declare_depth = declare_depth.saturating_add(1),
            "BEGIN" if declare_depth > 0 => declare_depth -= 1,
            _ => {}
        }
    }
    declare_depth > 0
}

fn inside_program_block(tokens: &[LexToken]) -> bool {
    let has_declare = tokens.iter().any(|token| is_word_token(token, "DECLARE"));
    if !has_declare {
        return false;
    }
    let mut depth = 0_u32;
    for token in tokens {
        if token.kind != TokenKind::Word {
            continue;
        }
        match token.text.to_ascii_uppercase().as_str() {
            "BEGIN" => depth = depth.saturating_add(1),
            "END" if depth > 0 => depth -= 1,
            _ => {}
        }
    }
    depth > 0
}

fn is_word_token(token: &LexToken, expected: &str) -> bool {
    token.kind == TokenKind::Word && token.text.eq_ignore_ascii_case(expected)
}

struct Lexed {
    tokens: Vec<LexToken>,
    state: LexState,
}

pub fn tokens(text: &str) -> Vec<LexToken> {
    lex_sql(text, 0).tokens
}

fn lex_sql(text: &str, base: usize) -> Lexed {
    let bytes = text.as_bytes();
    let mut tokens = Vec::new();
    let mut index = 0;
    let mut state = LexState::Code;
    while index < bytes.len() {
        match state {
            LexState::Code => {
                // Code-state positions are UTF-8 boundaries. Advance by a whole
                // scalar, including for unknown punctuation, before slicing tokens.
                let character = text[index..].chars().next().unwrap_or('\0');
                if character.is_whitespace() {
                    index += character.len_utf8();
                } else if bytes[index] == b'-' && bytes.get(index + 1) == Some(&b'-') {
                    state = LexState::LineComment;
                    index += 2;
                } else if bytes[index] == b'/' && bytes.get(index + 1) == Some(&b'*') {
                    state = LexState::BlockComment;
                    index += 2;
                } else if bytes[index] == b'\'' {
                    state = LexState::String;
                    index += 1;
                } else if bytes[index] == b'"' {
                    let start = index;
                    state = LexState::QuotedIdentifier;
                    index += 1;
                    while index < bytes.len() {
                        if bytes[index] == b'"' {
                            if bytes.get(index + 1) == Some(&b'"') {
                                index += 2;
                                continue;
                            }
                            index += 1;
                            state = LexState::Code;
                            break;
                        }
                        index += 1;
                    }
                    let value_end = if state == LexState::Code {
                        index.saturating_sub(1)
                    } else {
                        index
                    };
                    let value = text[start + 1..value_end].replace("\"\"", "\"");
                    tokens.push(LexToken {
                        kind: TokenKind::Word,
                        text: value,
                        start: base + start,
                        end: base + index,
                    });
                } else if is_identifier_start(character) {
                    let start = index;
                    index += character.len_utf8();
                    while let Some(next) = text[index..].chars().next() {
                        if !is_identifier_continue(next) {
                            break;
                        }
                        index += next.len_utf8();
                    }
                    tokens.push(LexToken {
                        kind: TokenKind::Word,
                        text: text[start..index].to_owned(),
                        start: base + start,
                        end: base + index,
                    });
                } else if bytes[index].is_ascii_digit() {
                    let start = index;
                    index += 1;
                    while index < bytes.len()
                        && (bytes[index].is_ascii_digit()
                            || matches!(bytes[index], b'.' | b'e' | b'E' | b'+' | b'-'))
                    {
                        index += 1;
                    }
                    tokens.push(LexToken {
                        kind: TokenKind::Number,
                        text: text[start..index].to_owned(),
                        start: base + start,
                        end: base + index,
                    });
                } else {
                    let start = index;
                    index += character.len_utf8();
                    if matches!(
                        (bytes[start], bytes.get(index).copied()),
                        (b':', Some(b'='))
                            | (b'=', Some(b'>'))
                            | (b'|', Some(b'|'))
                            | (b'<', Some(b'='))
                            | (b'>', Some(b'='))
                            | (b'<', Some(b'>'))
                            | (b'!', Some(b'='))
                    ) {
                        index += 1;
                    }
                    tokens.push(LexToken {
                        kind: TokenKind::Symbol,
                        text: text[start..index].to_owned(),
                        start: base + start,
                        end: base + index,
                    });
                }
            }
            LexState::String => {
                if bytes[index] == b'\'' {
                    if bytes.get(index + 1) == Some(&b'\'') {
                        index += 2;
                    } else {
                        state = LexState::Code;
                        index += 1;
                    }
                } else {
                    index += 1;
                }
            }
            LexState::LineComment => {
                if matches!(bytes[index], b'\r' | b'\n') {
                    state = LexState::Code;
                }
                index += 1;
            }
            LexState::BlockComment => {
                if bytes[index] == b'*' && bytes.get(index + 1) == Some(&b'/') {
                    state = LexState::Code;
                    index += 2;
                } else {
                    index += 1;
                }
            }
            LexState::QuotedIdentifier => {
                unreachable!("quoted identifiers are consumed in the code branch");
            }
        }
    }
    Lexed { tokens, state }
}

fn is_identifier_start(character: char) -> bool {
    character.is_alphabetic() || character == '_'
}

fn is_identifier_continue(character: char) -> bool {
    character.is_alphanumeric() || matches!(character, '_' | '$' | '#')
}

fn clamp_char_boundary(text: &str, mut offset: usize) -> usize {
    while offset > 0 && !text.is_char_boundary(offset) {
        offset -= 1;
    }
    offset
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn detects_alias_member_and_prefix() {
        let sql = "SELECT o.AM FROM ORDERS o";
        let probe = analyze(sql, "SELECT o.AM".len());
        assert_eq!(probe.context, CompletionContext::Member);
        assert_eq!(probe.qualifier.as_deref(), Some("o"));
        assert_eq!(probe.prefix, "AM");
    }

    #[test]
    fn does_not_complete_inside_comment_or_string() {
        assert!(analyze("SELECT '-- o.fake", 17).suppressed);
        assert!(analyze("SELECT * /* unfinished", 22).suppressed);
    }

    #[test]
    fn finds_relation_context() {
        let sql = "SELECT * FROM OR";
        let probe = analyze(sql, sql.len());
        assert_eq!(probe.context, CompletionContext::Relation);
        assert_eq!(probe.prefix, "OR");
    }

    #[test]
    fn rope_probe_preserves_statement_boundaries_and_absolute_offsets() {
        let sql = "SELECT 'a;b'; SELECT /* ; */ 表名.列";
        let rope = Rope::from_str(sql);
        for cursor in 0..=sql.len() {
            assert_eq!(analyze_rope(&rope, cursor), analyze(sql, cursor));
        }
        let probe = analyze_rope(&rope, sql.len());
        assert_eq!(probe.qualifier.as_deref(), Some("表名"));
        assert_eq!(&sql[probe.replace_start..], "列");

        let sql = format!("SELECT :{}", "a".repeat(MAX_BACKWARD_BYTES));
        let probe = analyze_rope(&Rope::from_str(&sql), sql.len());
        assert_eq!(probe.context, CompletionContext::BindVariable);
        assert_eq!(probe.replace_start, "SELECT :".len());
    }

    #[test]
    fn unicode_code_tokens_keep_utf8_boundaries() {
        let sql = "\u{feff}SELECT\u{a0}客户_1.名称，🙂 FROM 表名;";
        let values = tokens(sql);
        assert!(values.iter().any(|t| t.kind == TokenKind::Word && t.text == "客户_1"));
        assert!(values.iter().any(|t| t.kind == TokenKind::Word && t.text == "表名"));
        assert!(values.iter().any(|t| t.kind == TokenKind::Symbol && t.text == "🙂"));
        assert!(values.iter().any(|t| t.kind == TokenKind::Symbol && t.text == "，"));
        assert!(!values.iter().any(|t| t.text == "\u{a0}"));
        for token in values {
            assert!(sql.is_char_boundary(token.start) && sql.is_char_boundary(token.end));
            assert_eq!(&sql[token.start..token.end], token.text);
        }
        let prefix = "SELECT 客户_1.名";
        let probe = analyze(prefix, prefix.len());
        assert_eq!(probe.qualifier.as_deref(), Some("客户_1"));
        assert_eq!(probe.prefix, "名");
        assert_eq!(&prefix[probe.replace_start..], "名");
    }

    #[test]
    fn unicode_strings_comments_and_backwards_window_remain_safe() {
        let sql = "SELECT '中文🙂', \"列\"\"名\" /* 中文 */ FROM 表名 -- 注释\nWHERE 名称 = '值';";
        for cursor in 0..=sql.len() {
            let probe = analyze(sql, cursor);
            assert!(sql.is_char_boundary(probe.replace_start));
        }
        let long = format!("{} SELECT 表名.列", "中".repeat(MAX_BACKWARD_BYTES));
        let probe = analyze(&long, long.len());
        assert_eq!(analyze_rope(&Rope::from_str(&long), long.len()), probe);
        assert_eq!(probe.qualifier.as_deref(), Some("表名"));
        assert_eq!(probe.prefix, "列");
        assert!(analyze("SELECT '未结束🙂", "SELECT '未结束🙂".len()).suppressed);
        assert!(analyze("SELECT /* 未结束🙂", "SELECT /* 未结束🙂".len()).suppressed);
    }
}
