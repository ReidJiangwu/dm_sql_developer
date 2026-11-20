use std::collections::HashSet;
use std::sync::atomic::{AtomicBool, Ordering};

use serde::{Deserialize, Serialize};

#[derive(Clone, Copy, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub enum Preset {
    #[default]
    Standard,
    Compact,
    Expanded,
    Custom,
}

#[derive(Clone, Copy, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub enum CaseMode {
    #[default]
    Preserve,
    Upper,
    Lower,
}

#[derive(Clone, Copy, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub enum BreakMode {
    Preserve,
    #[default]
    NewLine,
}

#[derive(Clone, Copy, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub enum LogicalOperatorMode {
    Preserve,
    #[default]
    Leading,
    Trailing,
}

#[derive(Clone, Copy, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub enum CommaMode {
    #[default]
    Trailing,
    Leading,
}

#[derive(Clone, Copy, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub enum ListMode {
    Compact,
    #[default]
    WhenWrapped,
    OnePerLine,
}

#[derive(Clone, Copy, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub enum ParenthesesMode {
    #[default]
    Compact,
    Expanded,
}

#[derive(Clone, Copy, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub enum ErrorMode {
    #[default]
    Safe,
    Reject,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(default, rename_all = "camelCase")]
pub struct IndentOptions {
    pub size: u8,
    pub continuation_size: u8,
    pub use_tabs: bool,
}

impl Default for IndentOptions {
    fn default() -> Self {
        Self {
            size: 4,
            continuation_size: 4,
            use_tabs: false,
        }
    }
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(default, rename_all = "camelCase")]
pub struct CaseOptions {
    pub keywords: CaseMode,
    pub data_types: CaseMode,
    pub built_in_functions: CaseMode,
    pub tables: CaseMode,
    pub columns: CaseMode,
    pub sequences: CaseMode,
    pub routines: CaseMode,
    pub variables: CaseMode,
    pub aliases: CaseMode,
}

impl Default for CaseOptions {
    fn default() -> Self {
        Self {
            keywords: CaseMode::Upper,
            data_types: CaseMode::Upper,
            built_in_functions: CaseMode::Upper,
            tables: CaseMode::Preserve,
            columns: CaseMode::Preserve,
            sequences: CaseMode::Preserve,
            routines: CaseMode::Preserve,
            variables: CaseMode::Preserve,
            aliases: CaseMode::Preserve,
        }
    }
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(default, rename_all = "camelCase")]
pub struct LayoutOptions {
    pub max_line_width: u16,
    pub major_clauses: BreakMode,
    pub joins: BreakMode,
    pub logical_operators: LogicalOperatorMode,
    pub commas: CommaMode,
    pub select_items: ListMode,
    pub function_arguments: ListMode,
    pub parentheses: ParenthesesMode,
    pub preserve_user_line_breaks: bool,
}

impl Default for LayoutOptions {
    fn default() -> Self {
        Self {
            max_line_width: 100,
            major_clauses: BreakMode::NewLine,
            joins: BreakMode::NewLine,
            logical_operators: LogicalOperatorMode::Leading,
            commas: CommaMode::Trailing,
            select_items: ListMode::OnePerLine,
            function_arguments: ListMode::WhenWrapped,
            parentheses: ParenthesesMode::Compact,
            preserve_user_line_breaks: false,
        }
    }
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(default, rename_all = "camelCase")]
pub struct ProceduralOptions {
    pub declarations_one_per_line: bool,
    pub cursor_query_on_new_line: bool,
    pub then_on_new_line: bool,
    pub loop_on_new_line: bool,
}

impl Default for ProceduralOptions {
    fn default() -> Self {
        Self {
            declarations_one_per_line: true,
            cursor_query_on_new_line: true,
            then_on_new_line: false,
            loop_on_new_line: false,
        }
    }
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(default, rename_all = "camelCase")]
pub struct CommentOptions {
    pub preserve_content: bool,
    pub standalone_block_comments: BreakMode,
}

impl Default for CommentOptions {
    fn default() -> Self {
        Self {
            preserve_content: true,
            standalone_block_comments: BreakMode::Preserve,
        }
    }
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(default, rename_all = "camelCase")]
pub struct ErrorOptions {
    pub mode: ErrorMode,
}

impl Default for ErrorOptions {
    fn default() -> Self {
        Self {
            mode: ErrorMode::Safe,
        }
    }
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(default, rename_all = "camelCase")]
pub struct FormatterProfile {
    pub version: u8,
    pub preset: Preset,
    pub indent: IndentOptions,
    pub case: CaseOptions,
    pub layout: LayoutOptions,
    pub procedural: ProceduralOptions,
    pub comments: CommentOptions,
    pub errors: ErrorOptions,
}

impl Default for FormatterProfile {
    fn default() -> Self {
        Self {
            version: 1,
            preset: Preset::Standard,
            indent: IndentOptions::default(),
            case: CaseOptions::default(),
            layout: LayoutOptions::default(),
            procedural: ProceduralOptions::default(),
            comments: CommentOptions::default(),
            errors: ErrorOptions::default(),
        }
    }
}

impl FormatterProfile {
    pub fn resolved(mut self) -> Result<Self, FormatError> {
        if self.version != 1 {
            return Err(FormatError::InvalidProfile(format!(
                "unsupported formatter profile version: {}",
                self.version
            )));
        }
        if !(1..=16).contains(&self.indent.size)
            || !(1..=32).contains(&self.indent.continuation_size)
            || !(40..=400).contains(&self.layout.max_line_width)
        {
            return Err(FormatError::InvalidProfile(
                "formatter indentation or line width is outside the supported range".to_owned(),
            ));
        }
        match self.preset {
            Preset::Standard => self.layout.select_items = ListMode::OnePerLine,
            Preset::Custom => {}
            Preset::Compact => {
                self.layout.select_items = ListMode::Compact;
                self.layout.function_arguments = ListMode::Compact;
                self.layout.parentheses = ParenthesesMode::Compact;
                self.layout.max_line_width = self.layout.max_line_width.max(120);
            }
            Preset::Expanded => {
                self.layout.select_items = ListMode::OnePerLine;
                self.layout.function_arguments = ListMode::OnePerLine;
                self.layout.parentheses = ParenthesesMode::Expanded;
            }
        }
        Ok(self)
    }
}

#[derive(Debug, Eq, PartialEq)]
pub enum FormatError {
    Cancelled,
    InvalidProfile(String),
    SyntaxErrors,
    UnsafeTransformation,
}

impl std::fmt::Display for FormatError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Cancelled => formatter.write_str("formatting cancelled"),
            Self::InvalidProfile(message) => formatter.write_str(message),
            Self::SyntaxErrors => formatter.write_str("document contains syntax errors"),
            Self::UnsafeTransformation => formatter.write_str("formatting would change protected SQL content; original text retained"),
        }
    }
}

impl std::error::Error for FormatError {}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum TokenKind {
    Word,
    Number,
    String,
    QuotedIdentifier,
    Bind,
    Operator,
    Punctuation,
    LineComment,
    BlockComment,
    RawCommand,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
enum IdentifierRole {
    #[default]
    Unknown,
    Table,
    Column,
    Sequence,
    Routine,
    Variable,
    Alias,
}

#[derive(Clone, Debug)]
struct Token {
    kind: TokenKind,
    text: String,
    newline_before: bool,
    role: IdentifierRole,
}

#[derive(Clone, Copy, Debug, Default)]
struct Context {
    function: bool,
    select_list: bool,
    wrapped: bool,
    query: bool,
    indent: usize,
    clause: &'static str,
    select_depth: usize,
    list: bool,
    compact: bool,
}

#[derive(Clone, Copy)]
struct BlockContext {
    kind: &'static str,
    indent: usize,
}

pub fn format_sql(
    source: &str,
    profile: FormatterProfile,
    has_syntax_errors: bool,
    cancelled: Option<&AtomicBool>,
) -> Result<String, FormatError> {
    let profile = profile.resolved()?;
    if has_syntax_errors && profile.errors.mode == ErrorMode::Reject {
        return Err(FormatError::SyntaxErrors);
    }
    if has_unclosed_protected_region(source) {
        return Ok(source.to_owned());
    }
    if source.trim().is_empty() {
        return Ok(source.to_owned());
    }
    ensure_not_cancelled(cancelled)?;
    let eol = if source.contains("\r\n") {
        "\r\n"
    } else {
        "\n"
    };
    let final_newline = source.ends_with('\n') || source.ends_with('\r');
    let tokens = tokenize(source, cancelled)?;
    // Incomplete parentheses and unsupported conditional compilation must not be
    // guessed at. Parser gaps in complete DM SQL remain supported by token checks.
    let mut depth = 0usize;
    for token in &tokens {
        if token.text == "(" { depth += 1; }
        if token.text == ")" {
            if depth == 0 { return Ok(source.to_owned()); }
            depth -= 1;
        }
        if token.kind == TokenKind::Word && token.text.starts_with('$') {
            return Ok(source.to_owned());
        }
    }
    if depth != 0 { return Ok(source.to_owned()); }
    let mut writer = Writer::new(&profile, eol, cancelled);
    writer.write_tokens(&tokens)?;
    let mut output = writer.finish();
    while output.ends_with([' ', '\t', '\r', '\n']) {
        output.pop();
    }
    if final_newline {
        output.push_str(eol);
    }
    let after = tokenize(&output, cancelled)?;
    if tokens.len() != after.len() || tokens.iter().zip(&after).any(|(before, after)| {
        before.kind != after.kind || if before.kind == TokenKind::Word {
            !before.text.eq_ignore_ascii_case(&after.text)
        } else {
            before.text != after.text
        }
    }) {
        return Err(FormatError::UnsafeTransformation);
    }
    Ok(output)
}

fn has_unclosed_protected_region(source: &str) -> bool {
    #[derive(Clone, Copy, Eq, PartialEq)]
    enum State {
        Normal,
        SingleQuote,
        DoubleQuote,
        BlockComment,
        LineComment,
    }

    let bytes = source.as_bytes();
    let mut state = State::Normal;
    let mut index = 0;
    while index < bytes.len() {
        let value = bytes[index];
        let next = bytes.get(index + 1).copied();
        if state == State::Normal && matches!(value, b'q' | b'Q') && next == Some(b'\'') {
            let Some(end) = alternative_quote_end(bytes, index) else { return true; };
            index = end;
            continue;
        }
        match state {
            State::Normal if value == b'\'' => state = State::SingleQuote,
            State::Normal if value == b'"' => state = State::DoubleQuote,
            State::Normal if value == b'/' && next == Some(b'*') => {
                state = State::BlockComment;
                index += 1;
            }
            State::Normal if value == b'-' && next == Some(b'-') => {
                state = State::LineComment;
                index += 1;
            }
            State::SingleQuote if value == b'\'' && next == Some(b'\'') => index += 1,
            State::SingleQuote if value == b'\'' => state = State::Normal,
            State::DoubleQuote if value == b'"' && next == Some(b'"') => index += 1,
            State::DoubleQuote if value == b'"' => state = State::Normal,
            State::BlockComment if value == b'*' && next == Some(b'/') => {
                state = State::Normal;
                index += 1;
            }
            State::LineComment if value == b'\n' => state = State::Normal,
            _ => {}
        }
        index += 1;
    }
    matches!(state, State::SingleQuote | State::DoubleQuote | State::BlockComment)
}

/// Range edits require complete statement boundaries, not a guessed fragment
/// inside a string, expression, subquery or procedural block.
pub fn is_safe_range(source: &str, start: usize, end: usize) -> bool {
    if start > end || !source.is_char_boundary(start) || !source.is_char_boundary(end) {
        return false;
    }
    let Ok(whole) = tokenize(source, None) else { return false; };
    let Ok(prefix) = tokenize(&source[..start], None) else { return false; };
    let Ok(selected) = tokenize(&source[start..end], None) else { return false; };
    let Ok(suffix) = tokenize(&source[end..], None) else { return false; };
    let split: Vec<_> = prefix.iter().chain(&selected).chain(&suffix).collect();
    if whole.len() != split.len() || whole.iter().zip(split).any(|(a, b)| a.kind != b.kind || a.text != b.text) {
        return false;
    }
    let profile = FormatterProfile::default();
    let mut writer = Writer::new(&profile, "\n", None);
    if writer.write_tokens(&prefix).is_err() || !writer.parens.is_empty()
        || !writer.blocks.is_empty() || !writer.at_statement_start {
        return false;
    }
    if writer.write_tokens(&selected).is_err() || !writer.parens.is_empty() || !writer.blocks.is_empty() {
        return false;
    }
    suffix.is_empty() || writer.at_statement_start
}

struct Writer<'a> {
    profile: &'a FormatterProfile,
    eol: &'a str,
    output: String,
    line: String,
    indent: usize,
    parens: Vec<Context>,
    current_clause: &'static str,
    at_statement_start: bool,
    select_indents: Vec<usize>,
    cursor_query_indent: bool,
    declaration_section: bool,
    blocks: Vec<BlockContext>,
    routine_header: bool,
    cancelled: Option<&'a AtomicBool>,
}

impl<'a> Writer<'a> {
    fn new(profile: &'a FormatterProfile, eol: &'a str, cancelled: Option<&'a AtomicBool>) -> Self {
        Self {
            profile,
            eol,
            output: String::new(),
            line: String::new(),
            indent: 0,
            parens: Vec::new(),
            current_clause: "",
            at_statement_start: true,
            select_indents: Vec::new(),
            cursor_query_indent: false,
            declaration_section: false,
            blocks: Vec::new(),
            routine_header: false,
            cancelled,
        }
    }

    fn write_tokens(&mut self, tokens: &[Token]) -> Result<(), FormatError> {
        let mut index = 0;
        while index < tokens.len() {
            ensure_not_cancelled(self.cancelled)?;
            let token = &tokens[index];
            let upper = token.text.to_ascii_uppercase();
            let previous = index.checked_sub(1).and_then(|value| tokens.get(value));
            let next = tokens.get(index + 1);

            if token.kind == TokenKind::RawCommand {
                self.newline();
                self.push_exact(token.text.trim_end_matches(['\r', '\n']));
                self.newline();
                self.at_statement_start = true;
                index += 1;
                continue;
            }
            if token.kind == TokenKind::LineComment {
                if token.newline_before { self.newline(); }
                self.space();
                self.push_exact(&token.text);
                self.newline();
                index += 1;
                continue;
            }
            if token.kind == TokenKind::BlockComment {
                if self.profile.comments.standalone_block_comments == BreakMode::NewLine
                    && token.newline_before
                {
                    self.newline();
                } else {
                    self.space();
                }
                self.push_exact(&token.text);
                if token.newline_before {
                    self.newline();
                }
                index += 1;
                continue;
            }

            if self.profile.layout.preserve_user_line_breaks
                && token.newline_before
                && !matches!(token.text.as_str(), ")" | "," | ";" | ".")
                && previous.is_some_and(|value| !matches!(value.text.as_str(), "(" | "."))
            {
                self.newline();
            }

            if token.kind == TokenKind::Word {
                let qualified = previous.is_some_and(|value| value.text == ".")
                    || next.is_some_and(|value| value.text == ".");
                if !qualified && self.handle_structural_keyword(&upper, previous, next) {
                    index += 1;
                    continue;
                }
                let create_or_replace = upper == "OR"
                    && previous.is_some_and(|token| token.text.eq_ignore_ascii_case("CREATE"))
                    && next.is_some_and(|token| token.text.eq_ignore_ascii_case("REPLACE"));
                if matches!(upper.as_str(), "AND" | "OR")
                    && !qualified
                    && !create_or_replace
                    && !is_between_conjunction(tokens, index)
                {
                    self.write_logical_operator(&upper);
                    index += 1;
                    continue;
                }
            }

            match token.text.as_str() {
                "(" if next.is_some_and(|value| value.text == "+")
                    && tokens.get(index + 2).is_some_and(|value| value.text == ")")
                    && previous.is_some_and(|value| matches!(value.kind,
                        TokenKind::Word | TokenKind::QuotedIdentifier)) => {
                    // DM's column suffix is an outer-join marker, not a function argument.
                    self.space();
                    self.push_exact("(+)");
                    index += 3;
                    continue;
                }
                "(" => {
                    let statement_start = tokens[..index].iter().rposition(|t| t.text == ";")
                        .map_or(0, |position| position + 1);
                    let header = &tokens[statement_start..index];
                    let list = self.parens.is_empty()
                        && (self.current_clause == "VALUES"
                            || previous.is_some_and(|t| matches!(t.text.to_ascii_uppercase().as_str(), "RECORD" | "OBJECT"))
                            || (header.iter().any(|t| matches!(t.text.to_ascii_uppercase().as_str(),
                                "TABLE" | "PROCEDURE" | "FUNCTION" | "RECORD"))
                                && !header.iter().any(|t| matches!(t.text.to_ascii_uppercase().as_str(),
                                    "SELECT" | "BEGIN" | "AS" | "IS")))
                            || (header.iter().any(|t| t.text.eq_ignore_ascii_case("INSERT"))
                                && self.current_clause != "SELECT"));
                    let function = !list && previous.is_some_and(is_function_name);
                    let compact = previous.is_some_and(|t| {
                        data_type_set().contains(t.text.to_ascii_uppercase().as_str())
                            || t.text.eq_ignore_ascii_case("OVER")
                    });
                    let query = !function && next.is_some_and(|value| {
                        value.text.eq_ignore_ascii_case("SELECT")
                            || value.text.eq_ignore_ascii_case("WITH")
                    });
                    let expanded = (query || list
                        || (function
                            && self.profile.layout.function_arguments == ListMode::OnePerLine)
                        || self.profile.layout.parentheses == ParenthesesMode::Expanded)
                        && !compact && !next.is_some_and(|value| value.text == ")");
                    if !function && previous.is_some_and(needs_space_before_parenthesis) {
                        self.space();
                    }
                    self.push_exact("(");
                    let select_list = self.current_clause == "SELECT" && self.parens.is_empty();
                    self.parens.push(Context {
                        function,
                        select_list,
                        wrapped: expanded,
                        query,
                        indent: self.indent,
                        clause: self.current_clause,
                        select_depth: self.select_indents.len(),
                        list,
                        compact,
                    });
                    if query { self.current_clause = ""; }
                    if expanded {
                        self.indent += 1;
                        self.newline();
                    }
                }
                ")" => {
                    let context = self.parens.pop().unwrap_or_default();
                    self.select_indents.truncate(context.select_depth);
                    self.current_clause = context.clause;
                    if context.wrapped {
                        self.indent = context.indent;
                        self.newline();
                    }
                    self.trim_line_end();
                    self.push_exact(")");
                }
                "," => self.write_comma(),
                "." => {
                    self.trim_line_end();
                    self.push_exact(".");
                }
                ";" => {
                    if let Some(indent) = self.select_indents.first().copied() {
                        self.indent = indent;
                        self.select_indents.clear();
                    }
                    if self.cursor_query_indent {
                        self.indent = self.indent.saturating_sub(1);
                        self.cursor_query_indent = false;
                    }
                    self.trim_line_end();
                    self.push_exact(";");
                    self.newline();
                    self.at_statement_start = true;
                    self.current_clause = "";
                    self.routine_header = false;
                }
                "*" if previous.is_some_and(|value| value.text == ".") => {
                    self.trim_line_end();
                    self.push_exact("*");
                }
                operator if token.kind == TokenKind::Operator => {
                    self.space();
                    self.push_exact(operator);
                    self.space();
                }
                _ => {
                    if needs_word_space(previous, token) {
                        self.space();
                    }
                    let formatted = self.format_token(token, previous, next);
                    self.push_exact(&formatted);
                    self.at_statement_start = false;
                }
            }
            index += 1;
        }
        Ok(())
    }

    fn handle_structural_keyword(
        &mut self,
        upper: &str,
        previous: Option<&Token>,
        next: Option<&Token>,
    ) -> bool {
        if self.parens.last().is_some_and(|c| c.function || c.compact || c.list) {
            return false;
        }
        if self.parens.is_empty() && matches!(upper, "PROCEDURE" | "FUNCTION" | "PACKAGE") {
            self.routine_header = true;
        }
        if self.parens.is_empty() && self.routine_header && matches!(upper, "AS" | "IS") {
            self.push_word(&apply_case(upper, self.profile.case.keywords));
            self.blocks.push(BlockContext { kind: "DECLARATION", indent: self.indent });
            self.indent += 1;
            self.newline();
            self.declaration_section = true;
            self.routine_header = false;
            self.at_statement_start = true;
            return true;
        }
        if upper == "EXCEPTION" && self.parens.is_empty() {
            if let Some(block) = self.blocks.last_mut() {
                block.kind = "EXCEPTION";
                self.indent = block.indent;
                self.newline();
                self.push_word(&apply_case(upper, self.profile.case.keywords));
                self.indent += 1;
                self.newline();
                return true;
            }
        }
        // END IF/LOOP/CASE is one closing phrase, not a new block.
        if previous.is_some_and(|t| t.text.eq_ignore_ascii_case("END"))
            && matches!(upper, "IF" | "LOOP" | "CASE") {
            self.push_word(&apply_case(upper, self.profile.case.keywords));
            return true;
        }
        if upper == "END" {
            if let Some(block) = self.blocks.pop() {
                self.indent = block.indent;
                self.declaration_section = self.blocks.last().is_some_and(|b| b.kind == "DECLARATION");
                self.newline();
                self.push_word(&apply_case(upper, self.profile.case.keywords));
                self.at_statement_start = false;
                return true;
            }
        }
        if upper == "CASE" || (upper == "IF" && self.at_statement_start)
            || (upper == "LOOP" && self.parens.is_empty()) {
            let kind = if upper == "CASE" { "CASE" } else if upper == "IF" { "IF" } else { "LOOP" };
            if kind == "IF" { self.newline(); }
            self.blocks.push(BlockContext { kind, indent: self.indent });
            self.push_word(&apply_case(upper, self.profile.case.keywords));
            if kind == "LOOP" {
                self.indent += 1;
                self.newline();
                self.at_statement_start = true;
            } else {
                self.at_statement_start = false;
            }
            return true;
        }
        if matches!(upper, "WHEN" | "ELSE" | "ELSIF") {
            if let Some(block) = self.blocks.last().copied() {
                if matches!(block.kind, "CASE" | "IF" | "EXCEPTION") {
                    self.indent = block.indent + usize::from(block.kind != "IF");
                    self.newline();
                    self.push_word(&apply_case(upper, self.profile.case.keywords));
                    if upper == "ELSE" && block.kind == "IF" {
                        self.indent += 1;
                        self.newline();
                    }
                    self.at_statement_start = upper == "ELSE";
                    return true;
                }
            }
        }
        if upper == "THEN" && self.blocks.last().is_some_and(|b| matches!(b.kind, "IF" | "EXCEPTION")) {
            if self.profile.procedural.then_on_new_line { self.newline(); }
            self.push_word(&apply_case(upper, self.profile.case.keywords));
            self.indent += 1;
            self.newline();
            self.at_statement_start = true;
            return true;
        }
        if upper == "WHEN" && self.blocks.is_empty() && self.parens.is_empty() {
            if self.current_clause == "SET" {
                if let Some(indent) = self.select_indents.pop() { self.indent = indent; }
            }
            self.newline();
            self.push_word(&apply_case(upper, self.profile.case.keywords));
            self.current_clause = "";
            self.at_statement_start = false;
            return true;
        }
        if upper == "SELECT" {
            if let Some(context) = self.parens.last_mut() {
                context.query = true;
            }
        }
        let compound_major = matches!(upper, "GROUP" | "ORDER")
            && next.is_some_and(|token| token.text.eq_ignore_ascii_case("BY"))
            && (self.parens.is_empty() || self.parens.last().is_some_and(|context| context.query));
        let fetch_limit = upper == "FETCH"
            && next.is_some_and(|token| {
                matches!(token.text.to_ascii_uppercase().as_str(), "FIRST" | "NEXT")
            })
            && (self.parens.is_empty() || self.parens.last().is_some_and(|context| context.query));
        let major = matches!(
            upper,
            "SELECT"
                | "FROM"
                | "WHERE"
                | "HAVING"
                | "LIMIT"
                | "OFFSET"
                | "RETURNING"
                | "UNION"
                | "INTERSECT"
                | "EXCEPT"
                | "MINUS"
                | "MERGE"
                | "VALUES"
                | "WITH"
        ) || compound_major
            || fetch_limit
            || (upper == "USING" && !next.is_some_and(|t| t.text.eq_ignore_ascii_case("JAVA")))
            || (upper == "SET" && self.parens.is_empty());
        let previous_is_join_modifier = previous.is_some_and(|token| {
            matches!(
                token.text.to_ascii_uppercase().as_str(),
                "INNER" | "LEFT" | "RIGHT" | "FULL" | "CROSS" | "NATURAL" | "OUTER"
            )
        });
        let join = (upper == "JOIN" && !previous_is_join_modifier)
            || (matches!(
                upper,
                "INNER" | "LEFT" | "RIGHT" | "FULL" | "CROSS" | "NATURAL"
            ) && next.is_some_and(|token| {
                matches!(token.text.to_ascii_uppercase().as_str(), "JOIN" | "OUTER")
            }));
        let procedural_close = matches!(upper, "END" | "ELSE" | "ELSIF" | "EXCEPTION");
        let procedural_open = matches!(upper, "DECLARE" | "BEGIN");
        let statement_keyword = matches!(
            upper,
            "OPEN" | "FETCH" | "CLOSE" | "RETURN" | "RAISE" | "EXECUTE"
        );

        if procedural_close && self.parens.is_empty() {
            self.indent = self.indent.saturating_sub(1);
            self.newline();
            self.push_word(&apply_case(upper, self.profile.case.keywords));
            if upper != "END" {
                self.indent += 1;
            }
            self.at_statement_start = false;
            return true;
        }
        if procedural_open && self.parens.is_empty() {
            if upper == "BEGIN" && (self.declaration_section
                || self.blocks.last().is_some_and(|b| b.kind == "DECLARATION")) {
                self.indent = self.indent.saturating_sub(1);
                self.declaration_section = false;
                if self.blocks.last().is_some_and(|b| b.kind == "DECLARATION") {
                    self.blocks.pop();
                }
            }
            self.newline();
            if matches!(upper, "BEGIN" | "DECLARE") {
                self.blocks.push(BlockContext {
                    kind: if upper == "BEGIN" { "BEGIN" } else { "DECLARATION" },
                    indent: self.indent,
                });
            }
            self.push_word(&apply_case(upper, self.profile.case.keywords));
            self.indent += 1;
            self.newline();
            if upper == "DECLARE" {
                self.declaration_section = true;
            }
            self.at_statement_start = true;
            return true;
        }
        if (major && self.profile.layout.major_clauses == BreakMode::NewLine)
            || (join && self.profile.layout.joins == BreakMode::NewLine)
        {
            if upper == "SELECT"
                && previous.is_some_and(|token| token.text.eq_ignore_ascii_case("IS"))
                && self.profile.procedural.cursor_query_on_new_line
            {
                self.indent += 1;
                self.cursor_query_indent = true;
            }
            if upper == "FROM" || (matches!(self.current_clause, "SELECT" | "SET")
                && matches!(upper, "WHERE" | "RETURNING" | "UNION" | "INTERSECT" | "EXCEPT" | "MINUS")) {
                if let Some(indent) = self.select_indents.pop() {
                    self.indent = indent;
                }
            }
            self.newline();
            self.push_word(&apply_case(upper, self.profile.case.keywords));
            self.current_clause = if join { "JOIN" } else { clause_name(upper) };
            if (upper == "SELECT" && self.profile.layout.select_items == ListMode::OnePerLine)
                || upper == "SET" {
                self.select_indents.push(self.indent);
                self.indent += 1;
                self.newline();
            }
            self.at_statement_start = false;
            return true;
        }
        if statement_keyword && self.at_statement_start && self.parens.is_empty() {
            self.newline();
            self.push_word(&apply_case(upper, self.profile.case.keywords));
            self.at_statement_start = false;
            return true;
        }
        false
    }

    fn write_logical_operator(&mut self, upper: &str) {
        match self.profile.layout.logical_operators {
            LogicalOperatorMode::Leading => {
                self.newline();
                self.push_word(&apply_case(upper, self.profile.case.keywords));
            }
            LogicalOperatorMode::Trailing => {
                self.space();
                self.push_exact(&apply_case(upper, self.profile.case.keywords));
                self.newline();
            }
            LogicalOperatorMode::Preserve => {
                self.space();
                self.push_exact(&apply_case(upper, self.profile.case.keywords));
                self.space();
            }
        }
    }

    fn write_comma(&mut self) {
        self.trim_line_end();
        let context = self.parens.last().copied().unwrap_or_default();
        let mode = if context.compact {
            ListMode::Compact
        } else if context.list || (matches!(self.current_clause, "SET" | "WITH") && self.parens.is_empty()) {
            ListMode::OnePerLine
        } else if context.function {
            self.profile.layout.function_arguments
        } else if self.current_clause == "SELECT"
            && (self.parens.is_empty() || context.query)
        {
            self.profile.layout.select_items
        } else {
            ListMode::Compact
        };
        let break_list = mode == ListMode::OnePerLine
            || (mode == ListMode::WhenWrapped
                && self.line.chars().count() >= usize::from(self.profile.layout.max_line_width));
        if break_list && mode == ListMode::WhenWrapped && context.function && !context.wrapped {
            if let Some(active) = self.parens.last_mut() {
                active.wrapped = true;
            }
            self.indent += 1;
        }
        match self.profile.layout.commas {
            CommaMode::Trailing => {
                self.push_exact(",");
                if break_list {
                    self.newline();
                } else {
                    self.space();
                }
            }
            CommaMode::Leading if break_list || context.select_list => {
                self.newline();
                self.push_exact(",");
                self.space();
            }
            CommaMode::Leading => {
                self.push_exact(",");
                self.space();
            }
        }
    }

    fn format_token(
        &self,
        token: &Token,
        previous: Option<&Token>,
        next: Option<&Token>,
    ) -> String {
        if token.kind != TokenKind::Word {
            return token.text.clone();
        }
        let upper = token.text.to_ascii_uppercase();
        if keyword_set().contains(upper.as_str()) {
            return apply_case(&token.text, self.profile.case.keywords);
        }
        if data_type_set().contains(upper.as_str()) {
            return apply_case(&token.text, self.profile.case.data_types);
        }
        if next.is_some_and(|value| value.text == "(") {
            if built_in_function_set().contains(upper.as_str()) {
                return apply_case(&token.text, self.profile.case.built_in_functions);
            }
            return apply_case(&token.text, self.profile.case.routines);
        }
        if matches!(upper.as_str(), "NEXTVAL" | "CURRVAL")
            && previous.is_some_and(|value| value.text == ".")
        {
            return apply_case(&token.text, self.profile.case.keywords);
        }
        let mode = match token.role {
            IdentifierRole::Table => self.profile.case.tables,
            IdentifierRole::Column => self.profile.case.columns,
            IdentifierRole::Sequence => self.profile.case.sequences,
            IdentifierRole::Routine => self.profile.case.routines,
            IdentifierRole::Variable => self.profile.case.variables,
            IdentifierRole::Alias => self.profile.case.aliases,
            IdentifierRole::Unknown => CaseMode::Preserve,
        };
        apply_case(&token.text, mode)
    }

    fn push_word(&mut self, value: &str) {
        self.space();
        self.push_exact(value);
        self.space();
    }

    fn push_exact(&mut self, value: &str) {
        if self.line.is_empty() {
            self.line.push_str(&self.indent_text());
        }
        self.line.push_str(value);
    }

    fn space(&mut self) {
        if !self.line.is_empty() && !self.line.ends_with([' ', '\t']) {
            self.line.push(' ');
        }
    }

    fn trim_line_end(&mut self) {
        while self.line.ends_with([' ', '\t']) {
            self.line.pop();
        }
    }

    fn newline(&mut self) {
        self.trim_line_end();
        if self.line.trim().is_empty() {
            self.line.clear();
            return;
        }
        if !self.output.is_empty() {
            self.output.push_str(self.eol);
        }
        self.output.push_str(&self.line);
        self.line.clear();
    }

    fn indent_text(&self) -> String {
        if self.profile.indent.use_tabs {
            "\t".repeat(self.indent)
        } else {
            " ".repeat(self.indent * usize::from(self.profile.indent.size))
        }
    }

    fn finish(mut self) -> String {
        self.newline();
        self.output
    }
}

fn tokenize(source: &str, cancelled: Option<&AtomicBool>) -> Result<Vec<Token>, FormatError> {
    let mut tokens = Vec::new();
    let bytes = source.as_bytes();
    let mut index = 0;
    let mut newline_before = false;
    let mut line_start = true;
    while index < bytes.len() {
        ensure_not_cancelled(cancelled)?;
        let value = bytes[index];
        if value.is_ascii_whitespace() {
            if value == b'\n' {
                newline_before = true;
                line_start = true;
            }
            index += 1;
            continue;
        }
        if line_start {
            if let Some(end) = command_line_end(source, index) {
                tokens.push(Token {
                    kind: TokenKind::RawCommand,
                    text: source[index..end].trim_end_matches('\r').to_owned(),
                    newline_before,
                    role: IdentifierRole::Unknown,
                });
                newline_before = true;
                line_start = true;
                index = end;
                continue;
            }
        }
        line_start = false;
        let start = index;
        let kind = if matches!(value, b'q' | b'Q') && bytes.get(index + 1) == Some(&b'\'') {
            let Some(end) = alternative_quote_end(bytes, index) else {
                return Err(FormatError::SyntaxErrors);
            };
            index = end;
            TokenKind::String
        } else if value == b'\'' {
            index = quoted_end(bytes, index, b'\'');
            TokenKind::String
        } else if value == b'"' {
            index = quoted_end(bytes, index, b'"');
            TokenKind::QuotedIdentifier
        } else if value == b'-' && bytes.get(index + 1) == Some(&b'-') {
            index += 2;
            while index < bytes.len() && !matches!(bytes[index], b'\r' | b'\n') {
                index += 1;
            }
            TokenKind::LineComment
        } else if value == b'/' && bytes.get(index + 1) == Some(&b'*') {
            index += 2;
            while index + 1 < bytes.len() && !(bytes[index] == b'*' && bytes[index + 1] == b'/') {
                index += 1;
            }
            index = (index + 2).min(bytes.len());
            TokenKind::BlockComment
        } else if value == b':' && bytes.get(index + 1).is_some_and(u8::is_ascii_alphanumeric) {
            index += 2;
            while index < bytes.len()
                && (bytes[index].is_ascii_alphanumeric()
                    || matches!(bytes[index], b'_' | b'$' | b'#'))
            {
                index += 1;
            }
            TokenKind::Bind
        } else if source[index..].chars().next().is_some_and(|c| c.is_alphabetic())
            || matches!(value, b'_' | b'$' | b'#') {
            index += source[index..].chars().next().unwrap().len_utf8();
            while let Some(c) = source[index..].chars().next() {
                if !c.is_alphanumeric() && !matches!(c, '_' | '$' | '#') { break; }
                index += c.len_utf8();
            }
            TokenKind::Word
        } else if value.is_ascii_digit() {
            index += 1;
            while index < bytes.len() && bytes[index].is_ascii_digit() {
                index += 1;
            }
            if bytes.get(index) == Some(&b'.') && bytes.get(index + 1).is_some_and(u8::is_ascii_digit) {
                index += 1;
                while index < bytes.len() && bytes[index].is_ascii_digit() { index += 1; }
            }
            if bytes.get(index).is_some_and(|b| matches!(b, b'e' | b'E')) {
                let exponent = index;
                index += 1;
                if bytes.get(index).is_some_and(|b| matches!(b, b'+' | b'-')) { index += 1; }
                let digits = index;
                while index < bytes.len() && bytes[index].is_ascii_digit() { index += 1; }
                if digits == index { index = exponent; }
            }
            TokenKind::Number
        } else if let Some(width) = operator_width(bytes, index) {
            index += width;
            TokenKind::Operator
        } else {
            let character = source[index..]
                .chars()
                .next()
                .expect("index is inside source");
            index += character.len_utf8();
            TokenKind::Punctuation
        };
        tokens.push(Token {
            kind,
            text: source[start..index].to_owned(),
            newline_before,
            role: IdentifierRole::Unknown,
        });
        newline_before = false;
    }
    classify_identifiers(&mut tokens);
    Ok(tokens)
}

fn classify_identifiers(tokens: &mut [Token]) {
    for index in 0..tokens.len() {
        if !is_identifier(&tokens[index]) {
            continue;
        }
        if tokens.get(index + 1).is_some_and(|token| token.text == "(") {
            tokens[index].role = IdentifierRole::Routine;
        }
        if previous_word(tokens, index).is_some_and(|word| word.eq_ignore_ascii_case("AS")) {
            tokens[index].role = IdentifierRole::Alias;
        }
        if previous_word(tokens, index)
            .is_some_and(|word| matches!(word.to_ascii_uppercase().as_str(), "CURSOR" | "FETCH"))
        {
            tokens[index].role = IdentifierRole::Variable;
        }
    }

    for index in 0..tokens.len() {
        if !is_identifier(&tokens[index]) {
            continue;
        }
        if tokens.get(index + 1).is_some_and(|token| token.text == ".")
            && tokens.get(index + 2).is_some_and(is_nextval)
        {
            tokens[index].role = IdentifierRole::Sequence;
        }
        if tokens.get(index + 1).is_some_and(|token| token.text == ".")
            && tokens.get(index + 2).is_some_and(is_identifier)
            && tokens.get(index + 3).is_some_and(|token| token.text == ".")
            && tokens.get(index + 4).is_some_and(is_nextval)
        {
            tokens[index].role = IdentifierRole::Table;
            tokens[index + 2].role = IdentifierRole::Sequence;
        }
    }

    let mut index = 0;
    while index < tokens.len() {
        if relation_keyword(tokens, index) {
            let mut relation = index + 1;
            while relation < tokens.len() && matches!(tokens[relation].text.as_str(), "(" | ")") {
                relation += 1;
            }
            if is_identifier(tokens.get(relation).unwrap_or(&tokens[index])) {
                tokens[relation].role = IdentifierRole::Table;
                while relation + 2 < tokens.len()
                    && tokens[relation + 1].text == "."
                    && is_identifier(&tokens[relation + 2])
                {
                    relation += 2;
                    tokens[relation].role = IdentifierRole::Table;
                }
                let alias = relation + 1;
                if alias < tokens.len()
                    && is_identifier(&tokens[alias])
                    && tokens[alias].role == IdentifierRole::Unknown
                {
                    tokens[alias].role = IdentifierRole::Alias;
                }
            }
        }
        index += 1;
    }

    let mut variable_list = false;
    for index in 0..tokens.len() {
        let upper = tokens[index].text.to_ascii_uppercase();
        if tokens[index].kind == TokenKind::Word
            && upper == "INTO"
            && !previous_word(tokens, index).is_some_and(|word| {
                matches!(word.to_ascii_uppercase().as_str(), "INSERT" | "MERGE")
            })
        {
            variable_list = true;
            continue;
        }
        if variable_list {
            if is_identifier(&tokens[index]) {
                tokens[index].role = IdentifierRole::Variable;
            }
            if tokens[index].kind == TokenKind::Word
                && matches!(
                    upper.as_str(),
                    "FROM" | "USING" | "RETURNING" | "WHERE" | "LOOP"
                )
                || matches!(tokens[index].text.as_str(), ";")
            {
                variable_list = false;
            }
        }
    }

    for index in 0..tokens.len() {
        if !is_identifier(&tokens[index]) || tokens[index].role != IdentifierRole::Unknown {
            continue;
        }
        let previous = index.checked_sub(1).and_then(|value| tokens.get(value));
        let next = tokens.get(index + 1);
        let implicit_alias = next
            .is_some_and(|token| token.text == "," || token.text.eq_ignore_ascii_case("FROM"))
            && previous.is_some_and(|token| {
                !matches!(token.text.as_str(), "(" | "," | ".")
                    && !token.text.eq_ignore_ascii_case("SELECT")
            });
        tokens[index].role = if implicit_alias {
            IdentifierRole::Alias
        } else {
            IdentifierRole::Column
        };
    }
}

fn is_identifier(token: &Token) -> bool {
    if token.kind != TokenKind::Word {
        return false;
    }
    let upper = token.text.to_ascii_uppercase();
    !keyword_set().contains(upper.as_str()) && !data_type_set().contains(upper.as_str())
}

fn is_nextval(token: &Token) -> bool {
    matches!(
        token.text.to_ascii_uppercase().as_str(),
        "NEXTVAL" | "CURRVAL"
    )
}

fn previous_word(tokens: &[Token], index: usize) -> Option<&str> {
    tokens[..index]
        .iter()
        .rev()
        .find(|token| token.kind == TokenKind::Word)
        .map(|token| token.text.as_str())
}

fn relation_keyword(tokens: &[Token], index: usize) -> bool {
    if tokens[index].kind != TokenKind::Word {
        return false;
    }
    match tokens[index].text.to_ascii_uppercase().as_str() {
        "FROM" | "JOIN" | "UPDATE" | "USING" => true,
        "INTO" => previous_word(tokens, index)
            .is_some_and(|word| matches!(word.to_ascii_uppercase().as_str(), "INSERT" | "MERGE")),
        _ => false,
    }
}

fn alternative_quote_end(bytes: &[u8], start: usize) -> Option<usize> {
    let open = *bytes.get(start + 2)?;
    let close = match open { b'[' => b']', b'(' => b')', b'{' => b'}', b'<' => b'>', other => other };
    (start + 3..bytes.len().saturating_sub(1))
        .find(|&i| bytes[i] == close && bytes[i + 1] == b'\'')
        .map(|i| i + 2)
}

fn quoted_end(bytes: &[u8], mut index: usize, quote: u8) -> usize {
    index += 1;
    while index < bytes.len() {
        if bytes[index] == quote {
            if bytes.get(index + 1) == Some(&quote) {
                index += 2;
            } else {
                return index + 1;
            }
        } else {
            index += 1;
        }
    }
    index
}

fn operator_width(bytes: &[u8], index: usize) -> Option<usize> {
    const TWO: [&[u8]; 7] = [b":=", b"=>", b"||", b"<=", b">=", b"<>", b"!="];
    if TWO
        .iter()
        .any(|operator| bytes.get(index..index + 2) == Some(*operator))
    {
        return Some(2);
    }
    matches!(
        bytes[index],
        b'=' | b'<' | b'>' | b'+' | b'-' | b'*' | b'/' | b'%'
    )
    .then_some(1)
}

fn command_line_end(source: &str, start: usize) -> Option<usize> {
    let end = source[start..]
        .find('\n')
        .map_or(source.len(), |offset| start + offset);
    let line = source[start..end].trim_start();
    let upper = line.to_ascii_uppercase();
    let normalized = upper.split_whitespace().collect::<Vec<_>>().join(" ");
    let command = upper.trim_end() == "/" || upper.starts_with('@')
        || [
            "PROMPT ",
            "DEFINE ",
            "UNDEFINE ",
            "ACCEPT ",
            "SPOOL ",
            "DESC ",
            "DESCRIBE ",
            "WHENEVER SQLERROR ",
            "SET DEFINE ",
            "SET SERVEROUTPUT ",
            "SET LINESIZE ",
        ]
        .iter()
        .any(|prefix| normalized == prefix.trim_end() || normalized.starts_with(prefix));
    command.then_some(end)
}

fn needs_word_space(previous: Option<&Token>, current: &Token) -> bool {
    let Some(previous) = previous else {
        return false;
    };
    if matches!(current.text.as_str(), ")" | "," | ";" | ".") || previous.text == "." {
        return false;
    }
    !matches!(previous.text.as_str(), "(")
}

fn is_between_conjunction(tokens: &[Token], index: usize) -> bool {
    let mut depth = 0usize;
    for token in tokens[..index].iter().rev() {
        if token.text == ")" { depth += 1; continue; }
        if token.text == "(" {
            if depth == 0 { return false; }
            depth -= 1;
            continue;
        }
        if depth > 0 { continue; }
        let upper = token.text.to_ascii_uppercase();
        if upper == "BETWEEN" {
            return true;
        }
        if matches!(
            upper.as_str(),
            "AND" | "OR" | "WHERE" | "ON" | "HAVING" | "WHEN"
        ) || matches!(token.text.as_str(), "," | ";")
        {
            return false;
        }
    }
    false
}

fn is_function_name(token: &Token) -> bool {
    if token.kind != TokenKind::Word && token.kind != TokenKind::QuotedIdentifier {
        return false;
    }
    !keyword_set().contains(token.text.to_ascii_uppercase().as_str())
}

fn needs_space_before_parenthesis(token: &Token) -> bool {
    token.kind == TokenKind::Word
        && matches!(
            token.text.to_ascii_uppercase().as_str(),
            "IN" | "VALUES" | "OVER" | "AS" | "BY" | "USING"
        )
}

fn clause_name(upper: &str) -> &'static str {
    match upper {
            "SELECT" => "SELECT",
        "FROM" => "FROM",
        "WHERE" => "WHERE",
        "HAVING" => "HAVING",
        "SET" => "SET",
        "VALUES" => "VALUES",
        "WITH" => "WITH",
        _ => "",
    }
}

fn apply_case(value: &str, mode: CaseMode) -> String {
    match mode {
        CaseMode::Preserve => value.to_owned(),
        CaseMode::Upper => value.to_uppercase(),
        CaseMode::Lower => value.to_lowercase(),
    }
}

fn keyword_set() -> &'static HashSet<&'static str> {
    use std::sync::OnceLock;
    static SET: OnceLock<HashSet<&'static str>> = OnceLock::new();
    SET.get_or_init(|| {
        [
            "ALL",
            "ALTER",
            "AND",
            "AS",
            "ASC",
            "BEGIN",
            "BETWEEN",
            "BODY",
            "BY",
            "CASE",
            "CLOSE",
            "CONNECT",
            "CREATE",
            "CROSS",
            "CURRENT",
            "CURSOR",
            "DECLARE",
            "DEFAULT",
            "DELETE",
            "DESC",
            "DISTINCT",
            "DROP",
            "ELSE",
            "ELSIF",
            "END",
            "EXCEPT",
            "EXCEPTION",
            "EXECUTE",
            "FETCH",
            "FIRST",
            "FOLLOWING",
            "FOR",
            "FROM",
            "FULL",
            "FUNCTION",
            "GROUP",
            "HAVING",
            "IF",
            "IN",
            "INNER",
            "INSERT",
            "INTERSECT",
            "INTO",
            "IS",
            "JOIN",
            "LAST",
            "LEFT",
            "LIKE",
            "LIMIT",
            "LOOP",
            "MATCHED",
            "MERGE",
            "MINUS",
            "NATURAL",
            "NEXT",
            "NEXTVAL",
            "NOCYCLE",
            "NOT",
            "NULL",
            "NULLS",
            "OFFSET",
            "ON",
            "ONLY",
            "OPEN",
            "OR",
            "ORDER",
            "OUT",
            "OUTER",
            "OVER",
            "PACKAGE",
            "PARTITION",
            "PIVOT",
            "PRECEDING",
            "PRIOR",
            "PROCEDURE",
            "RAISE",
            "RANGE",
            "RECURSIVE",
            "REPLACE",
            "RETURN",
            "RETURNING",
            "RIGHT",
            "ROW",
            "ROWS",
            "SELECT",
            "SEQUENCE",
            "SET",
            "SIBLINGS",
            "START",
            "TABLE",
            "THEN",
            "TRIGGER",
            "UNBOUNDED",
            "UNION",
            "UNIQUE",
            "UNPIVOT",
            "UPDATE",
            "USING",
            "VALUES",
            "VIEW",
            "WHEN",
            "WHERE",
            "WHILE",
            "WITH",
        ]
        .into_iter()
        .collect()
    })
}

fn data_type_set() -> &'static HashSet<&'static str> {
    use std::sync::OnceLock;
    static SET: OnceLock<HashSet<&'static str>> = OnceLock::new();
    SET.get_or_init(|| {
        [
            "BIGINT",
            "BINARY",
            "BIT",
            "BLOB",
            "BOOLEAN",
            "CHAR",
            "CLOB",
            "CURSOR",
            "DATE",
            "DATETIME",
            "DEC",
            "DECIMAL",
            "DOUBLE",
            "FLOAT",
            "IMAGE",
            "INT",
            "INTEGER",
            "INTERVAL",
            "LONG",
            "NUMBER",
            "NUMERIC",
            "REAL",
            "SMALLINT",
            "SYS_REFCURSOR",
            "TEXT",
            "TIME",
            "TIMESTAMP",
            "TINYINT",
            "VARBINARY",
            "VARCHAR",
            "VARCHAR2",
        ]
        .into_iter()
        .collect()
    })
}

fn built_in_function_set() -> &'static HashSet<&'static str> {
    use std::sync::OnceLock;
    static SET: OnceLock<HashSet<&'static str>> = OnceLock::new();
    SET.get_or_init(|| {
        [
            "ABS",
            "AVG",
            "CAST",
            "CEIL",
            "COALESCE",
            "CONCAT",
            "COUNT",
            "DECODE",
            "DENSE_RANK",
            "EXTRACT",
            "FIRST_VALUE",
            "FLOOR",
            "JSON_ARRAY",
            "JSON_OBJECT",
            "LAG",
            "LAST_VALUE",
            "LEAD",
            "LENGTH",
            "LOWER",
            "LTRIM",
            "MAX",
            "MIN",
            "MOD",
            "NVL",
            "NVL2",
            "RANK",
            "REGEXP_REPLACE",
            "REPLACE",
            "ROUND",
            "ROW_NUMBER",
            "RTRIM",
            "SUBSTR",
            "SUM",
            "SYS_CONNECT_BY_PATH",
            "TO_CHAR",
            "TO_DATE",
            "TO_NUMBER",
            "TRIM",
            "TRUNC",
            "UPPER",
            "XMLAGG",
            "XMLELEMENT",
        ]
        .into_iter()
        .collect()
    })
}

fn ensure_not_cancelled(cancelled: Option<&AtomicBool>) -> Result<(), FormatError> {
    if cancelled.is_some_and(|flag| flag.load(Ordering::Relaxed)) {
        Err(FormatError::Cancelled)
    } else {
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn format(source: &str) -> String {
        format_sql(source, FormatterProfile::default(), false, None).unwrap()
    }

    #[test]
    fn object_matrix_preserves_tokens_and_is_idempotent() {
        let cases = [
            "create or replace view v as select id as \"Id\", api.\"Decode\"(state) as \"state\", (select max(x) from t2) as last_x, posted from jobs where id between 1 and 2;",
            "with a as (select id from t), b as (select id from a) select a.id,b.id from a left join b on a.id=b.id union all select id,id from a;",
            "update jobs set state='Ready', posted=sysdate where id=1; delete from jobs where id=2;",
            "insert into t (id,name) values (1,'中文');",
            "merge into t using s on (t.id=s.id) when matched then update set t.name=s.name when not matched then insert (id,name) values (s.id,s.name);",
            "create table t (id decimal(18,2), name varchar2(50), constraint pk primary key(id));",
            "create index ix on t(id,name); create sequence seq start with 1 increment by 1; grant select on t to u; comment on table t is 'Do not change';",
            "create or replace function f(p int,q int) return int is v int; begin if p>q then return p; else return q; end if; exception when others then raise; end;\n/",
            "create or replace package body p as procedure f is begin for i in 1..3 loop null; end loop; end; begin null; end;\n/",
            "create or replace trigger tr before insert on t for each row begin :new.id := seq.nextval; end;\n/",
            "select case when id=1 then 'one' else 'other' end label, count(*) over(partition by state order by id) n from t;",
            "DEFINE jar = '/opt/demo.jar'\ncreate or replace function f(p varchar2) return varchar2 external '&jar' \"demo.F.f\" using java;\nUNDEFINE jar\n/",
            "begin execute immediate q'[select 'a' from t -- keep]'; end;\n/",
            "-- standalone\r\nselect \"中文列\", '中文' from t; -- trailing\r\n",
        ];
        for source in cases {
            let once = format(source);
            assert_eq!(format(&once), once, "idempotence: {source}");
            let before = tokenize(source, None).unwrap();
            let after = tokenize(&once, None).unwrap();
            assert_eq!(before.len(), after.len(), "token count: {source}");
            for (a, b) in before.iter().zip(after) {
                assert_eq!(a.kind, b.kind);
                if a.kind == TokenKind::Word { assert!(a.text.eq_ignore_ascii_case(&b.text)); }
                else { assert_eq!(a.text, b.text); }
            }
        }
    }

    #[test]
    fn nested_query_restores_select_list_and_type_parameters_stay_compact() {
        let output = format("select (select max(x) from t2) last_x,id,posted from t;");
        assert!(output.contains(") last_x,\n    id,\n    posted\nFROM t;"));
        let ddl = format("create table t (id decimal(18,2), name varchar2(50));");
        assert!(ddl.contains("DECIMAL(18, 2),\n    name VARCHAR2(50)"));
    }

    #[test]
    fn range_requires_complete_statements_and_unknown_regions_are_preserved() {
        let source = "select a from t; select b from u;";
        assert!(is_safe_range(source, 0, 16));
        assert!(!is_safe_range(source, 7, 8));
        let quoted = "select 'select a from t;' from dual;";
        assert!(!is_safe_range(quoted, 8, 24));
        assert_eq!(format("select (id from t;"), "select (id from t;");
        assert_eq!(format("$IF x $THEN\nselect 1;\n$END"), "$IF x $THEN\nselect 1;\n$END");
        assert_eq!(format("select q'[unfinished"), "select q'[unfinished");
    }

    #[test]
    fn formats_query_and_preserves_protected_content() {
        assert_eq!(
            format("select 'from where' as txt /*+ keep */ from demo where id=1;"),
            "SELECT\n    'from where' AS txt /*+ keep */\nFROM demo\nWHERE id = 1;"
        );
    }

    #[test]
    fn formats_create_view_source() {
        assert_eq!(
            format(
                "create or replace view \"APP\".\"CODE_P\" as select company as \"COMPANY\",code_part_value as \"CODE_P\" from code_parts where enabled=1;",
            ),
            "CREATE OR REPLACE VIEW \"APP\".\"CODE_P\" AS\nSELECT\n    company AS \"COMPANY\",\n    code_part_value AS \"CODE_P\"\nFROM code_parts\nWHERE enabled = 1;"
        );
    }

    #[test]
    fn formats_qualified_wildcard_without_changing_multiplication() {
        let source = "SELECT a.*, a.id * b.factor FROM a, b;";
        let formatted = format(source);
        assert!(formatted.contains("a.*"), "{formatted}");
        assert!(formatted.contains("a.id * b.factor"), "{formatted}");
        assert_eq!(format(&formatted), formatted);
    }

    #[test]
    fn formats_outer_join_marker_without_expanding_or_spacing_its_operator() {
        let source = "SELECT a.id FROM a, b WHERE a.id = b.id(+) AND a.code = b.\"Code\" (+);";
        for preset in [Preset::Standard, Preset::Compact, Preset::Expanded] {
            let mut profile = FormatterProfile::default();
            profile.preset = preset;
            let formatted = format_sql(source, profile.clone(), false, None).unwrap();
            assert!(formatted.contains("b.id (+)"), "{preset:?}: {formatted}");
            assert!(formatted.contains("b.\"Code\" (+)"), "{preset:?}: {formatted}");
            assert_eq!(format_sql(&formatted, profile, false, None).unwrap(), formatted);
            let before = tokenize(source, None).unwrap();
            let after = tokenize(&formatted, None).unwrap();
            assert_eq!(before.len(), after.len());
            for (before, after) in before.iter().zip(after) {
                assert_eq!(before.kind, after.kind);
                assert!(before.text.eq_ignore_ascii_case(&after.text));
            }
        }
    }

    #[test]
    fn formats_dm_view_definition_with_nested_functions_and_outer_join_hint_safely() {
        let source = concat!(
            "CREATE OR REPLACE VIEW \"APP\".\"ACCESS_GROUP_OVW\" AS ",
            "SELECT R.ACCESS_GROUP_ID AS \"ACCESS_GROUP_ID\", ",
            "SUBSTR(NVL(EMPLOYEE_TRANSLATION_API.GET_BASIC_DATA_TRANSLATION(",
            "'PERSON','AccessRole',R.ACCESS_GROUP_ID),R.ACCESS_GROUP_NAME),1,200) ",
            "AS \"ACCESS_GROUP_NAME\", R.ACCESS_GROUP_DESCRIPTION AS ",
            "\"ACCESS_GROUP_DESCRIPTION\", R.ACCESS_GROUP_TYPE_DB AS ",
            "\"ACCESS_GROUP_TYPE_DB\" FROM ACCESS_GROUP_TAB R, ",
            "ACCESS_GROUP_ITEM_TAB I WHERE R.ACCESS_GROUP_ID = I.ACCESS_GROUP_ID (+);",
        );
        let formatted = format(source);
        assert!(formatted.contains("SUBSTR"));
        assert!(formatted.contains("NVL"));
        assert!(formatted.contains("EMPLOYEE_TRANSLATION_API.GET_BASIC_DATA_TRANSLATION"));
        assert!(formatted.contains("WHERE R.ACCESS_GROUP_ID = I.ACCESS_GROUP_ID (+);"));
        assert!(formatted.contains("\"ACCESS_GROUP_ID\""));
        let before = tokenize(source, None).unwrap();
        let after = tokenize(&formatted, None).unwrap();
        assert_eq!(before.len(), after.len());
        for (before, after) in before.iter().zip(after) {
            assert_eq!(before.kind, after.kind);
            if before.kind == TokenKind::Word {
                assert!(before.text.eq_ignore_ascii_case(&after.text));
            } else {
                assert_eq!(before.text, after.text);
            }
        }
        assert_eq!(format(&formatted), formatted, "view formatting must be idempotent");
    }

    #[test]
    fn preserves_line_endings_and_final_newline_state() {
        assert_eq!(
            format_sql(
                "select 1 from dual;\r\n",
                FormatterProfile::default(),
                false,
                None
            )
            .unwrap(),
            "SELECT\r\n    1\r\nFROM dual;\r\n"
        );
    }

    #[test]
    fn formatting_is_idempotent() {
        let once = format(
            "select sum(o.amount) over(partition by o.customer_id order by o.created_at) total from orders o where o.amount>0;",
        );
        assert_eq!(format(&once), once);
    }

    #[test]
    fn reject_mode_refuses_syntax_errors() {
        let mut profile = FormatterProfile::default();
        profile.errors.mode = ErrorMode::Reject;
        assert_eq!(
            format_sql("select (", profile, true, None),
            Err(FormatError::SyntaxErrors)
        );
    }

    #[test]
    fn safe_mode_preserves_documents_with_syntax_errors() {
        assert_eq!(
            format_sql(
                "select ('unfinished",
                FormatterProfile::default(),
                true,
                None
            )
            .unwrap(),
            "select ('unfinished"
        );
    }

    #[test]
    fn safe_mode_formats_lexically_complete_dm_sql_with_parser_gaps() {
        assert_eq!(
            format_sql(
                "select a,b from demo where id=1;",
                FormatterProfile::default(),
                true,
                None,
            )
            .unwrap(),
            "SELECT\n    a,\n    b\nFROM demo\nWHERE id = 1;"
        );
    }

    #[test]
    fn safe_mode_formats_complete_business_sql_with_bind_variables_and_offset_identifier() {
        let source = concat!(
            "SELECT B.etag, B.luname, B.keyref, B.objgrants, B.contract, B.company, ",
            "B.delivery_address, B.dist_calendar_id, B.manuf_calendar_id, B.\"OFFSET\" ",
            "FROM (SELECT contract contract, company company FROM COMPANY_SITE WHERE ",
            "((contract = :Contract_0))) A, (SELECT C.* FROM (SELECT contract contract, ",
            "company company, 'W/\"'||objid||':'||objversion||'\"' etag, 'Site' luname, ",
            "'COMPANY='||company||'^'||'CONTRACT='||contract||'^' keyref, NULL objgrants, ",
            "delivery_address delivery_address, dist_calendar_id dist_calendar_id, ",
            "manuf_calendar_id manuf_calendar_id, \"OFFSET\" \"OFFSET\" FROM SITE WHERE ",
            "((CONTRACT = :Contract_1 and COMPANY = :Company_1))) C) B WHERE ",
            "((A.contract = B.contract AND A.company = B.company))",
        );
        let formatted = format_sql(
            source,
            FormatterProfile::default(),
            true,
            None,
        )
        .unwrap();
        assert_ne!(formatted, source);
        assert!(formatted.lines().count() > 8);
        assert!(formatted.contains("B.\"OFFSET\""));
        assert!(formatted.contains(":Contract_0"));
        assert!(formatted.contains("'W/\"' || objid || ':' || objversion || '\"'"));
        assert!(formatted.contains("\nFROM ("));
        assert!(formatted.contains("\nWHERE "));
    }

    #[test]
    fn cancellation_is_observed() {
        let cancelled = AtomicBool::new(true);
        assert_eq!(
            format_sql(
                "select 1",
                FormatterProfile::default(),
                false,
                Some(&cancelled)
            ),
            Err(FormatError::Cancelled)
        );
    }

    #[test]
    fn command_lines_are_not_rewritten() {
        assert_eq!(
            format("PROMPT select from\nselect 1 from dual;"),
            "PROMPT select from\nSELECT\n    1\nFROM dual;"
        );
    }

    #[test]
    fn expanded_profile_formats_nested_functions() {
        let mut profile = FormatterProfile::default();
        profile.preset = Preset::Expanded;
        assert_eq!(
            format_sql(
                "select coalesce(trim(upper(c.name)),'N/A') display_name from customer c;",
                profile,
                false,
                None
            )
            .unwrap(),
            "SELECT\n    COALESCE(\n        TRIM(\n            UPPER(\n                c.name\n            )\n        ),\n        'N/A'\n    ) display_name\nFROM customer c;"
        );
    }

    #[test]
    fn formats_cursor_statements_and_sequence_members() {
        assert_eq!(
            format(
                "declare cursor c_order(p_id int) is select order_seq.nextval from orders where id=p_id; begin open c_order(1); fetch c_order into v_id; close c_order; end;"
            ),
            "DECLARE\n    CURSOR c_order(p_id INT) IS\n        SELECT\n            order_seq.NEXTVAL\n        FROM orders\n        WHERE id = p_id;\nBEGIN\n    OPEN c_order(1);\n    FETCH c_order INTO v_id;\n    CLOSE c_order;\nEND;"
        );
    }

    #[test]
    fn formats_generated_anonymous_block_with_keyword_prefixed_variables() {
        let source = "DECLARE return_array_ Manual_Supplier_Invoice_Handling_SVC.\
Posting_Structure_Art; cursor_ SYS_REFCURSOR; BEGIN return_array_ := \
Manual_Supplier_Invoice_Handling_SVC.Do_Post_Invoices('TESTGZYD003', 59, \
'TESTS1', 'SI', '2026080501', TO_DATE('2026-08-07','YYYY-MM-DD'), 'AC', 'I', \
NULL, 2026, 'FALSE', '1', 'Preliminary', 'FALSE', NULL, unbound## => ''); \
OPEN cursor_ FOR SELECT * FROM TABLE(return_array_); NULL := cursor_; END;";
        let formatted = format(source);

        assert!(formatted.starts_with("DECLARE\n    return_array_"));
        assert!(formatted.contains("\n    cursor_ SYS_REFCURSOR;"));
        assert!(formatted.contains("\nBEGIN\n    return_array_ :="));
        assert!(formatted.contains("\n    OPEN cursor_ FOR\n    SELECT\n        *\n    FROM TABLE(return_array_);"));
        assert!(formatted.contains("\n    NULL := cursor_;"));
        assert!(formatted.ends_with("\nEND;"));
        assert_eq!(format(&formatted), formatted);
    }

    #[test]
    fn profile_changes_keyword_and_identifier_safe_categories() {
        let mut profile = FormatterProfile::default();
        profile.case.keywords = CaseMode::Lower;
        profile.case.built_in_functions = CaseMode::Lower;
        assert_eq!(
            format_sql(
                "SELECT SUM(\"MixedName\") total FROM Orders;",
                profile,
                false,
                None
            )
            .unwrap(),
            "select\n    sum(\"MixedName\") total\nfrom Orders;"
        );
    }

    #[test]
    fn profile_applies_identifier_roles_without_touching_quoted_names() {
        let mut profile = FormatterProfile::default();
        profile.case.tables = CaseMode::Upper;
        profile.case.columns = CaseMode::Lower;
        profile.case.sequences = CaseMode::Upper;
        profile.case.routines = CaseMode::Lower;
        profile.case.aliases = CaseMode::Lower;
        assert_eq!(
            format_sql(
                "select s.order_seq.nextval as NewId, Fn_Custom(t.Amount) Total from Sales.Orders t where t.Id=Other_Id and t.Name=\"ExactName\";",
                profile,
                false,
                None
            )
            .unwrap(),
            "SELECT\n    S.ORDER_SEQ.NEXTVAL AS newid,\n    fn_custom(t.amount) total\nFROM SALES.ORDERS t\nWHERE t.id = other_id\nAND t.name = \"ExactName\";"
        );
    }

    #[test]
    fn profile_can_preserve_existing_line_breaks() {
        let mut profile = FormatterProfile::default();
        profile.layout.preserve_user_line_breaks = true;
        assert_eq!(
            format_sql("select a +\nb from demo;", profile, false, None).unwrap(),
            "SELECT\n    a +\n    b\nFROM demo;"
        );
    }
}
