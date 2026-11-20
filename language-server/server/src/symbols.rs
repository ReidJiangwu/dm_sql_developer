use serde::Serialize;
use tree_sitter::Node;

use crate::backward::{LexToken, TokenKind, tokens};
use crate::document::{Document, Range};

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DocumentSymbol {
    pub symbol_id: String,
    pub name: String,
    #[serde(skip_serializing_if = "String::is_empty")]
    pub detail: String,
    pub kind: u8,
    pub member_kind: String,
    pub source_unit: String,
    pub role: String,
    pub source_order: u32,
    #[serde(skip_serializing_if = "String::is_empty")]
    pub signature: String,
    #[serde(skip_serializing_if = "String::is_empty")]
    pub display_signature: String,
    #[serde(skip_serializing_if = "String::is_empty")]
    pub visibility: String,
    pub range: Range,
    pub selection_range: Range,
    #[serde(skip_serializing_if = "Vec::is_empty")]
    pub children: Vec<DocumentSymbol>,
}

pub fn document_symbols(document: &Document) -> Vec<DocumentSymbol> {
    let Some(tree) = &document.tree else {
        return Vec::new();
    };
    let source = document.source();
    let mut symbols = symbols_below(document, &source, tree.root_node());
    merge_fallback_package_routines(document, &source, &mut symbols);
    symbols
}

fn merge_fallback_package_routines(
    document: &Document,
    source: &str,
    symbols: &mut [DocumentSymbol],
) {
    let Some((source_unit, package_name, routines)) = fallback_package_routines(document, source)
    else {
        return;
    };
    let Some(package) = symbols.iter_mut().find(|symbol| {
        symbol.member_kind == "PACKAGE"
            && symbol.source_unit == source_unit
            && symbol.name.eq_ignore_ascii_case(&package_name)
    }) else {
        return;
    };
    for routine in routines {
        if package
            .children
            .iter()
            .any(|symbol| symbol.symbol_id == routine.symbol_id)
        {
            continue;
        }
        package.children.push(routine);
    }
    package.children.sort_by_key(|symbol| symbol.source_order);
}

fn fallback_package_routines(
    document: &Document,
    source: &str,
) -> Option<(String, String, Vec<DocumentSymbol>)> {
    let tokens = tokens(source);
    let package_index = tokens
        .iter()
        .position(|token| is_word(token, "PACKAGE"))?;
    let mut index = package_index + 1;
    let source_unit = if tokens.get(index).is_some_and(|token| is_word(token, "BODY")) {
        index += 1;
        "BODY"
    } else {
        "SPECIFICATION"
    };
    let package_name_start = tokens
        .get(index)
        .filter(|token| token.kind == TokenKind::Word)?
        .start;
    let mut package_name_end = tokens[index].end;
    index += 1;
    while index + 1 < tokens.len()
        && tokens[index].kind == TokenKind::Symbol
        && tokens[index].text == "."
        && tokens[index + 1].kind == TokenKind::Word
    {
        package_name_end = tokens[index + 1].end;
        index += 2;
    }
    let package_name = source
        .get(package_name_start..package_name_end)?
        .trim()
        .to_owned();
    while index < tokens.len()
        && !is_word(&tokens[index], "AS")
        && !is_word(&tokens[index], "IS")
    {
        index += 1;
    }
    if index == tokens.len() {
        return None;
    }
    index += 1;

    let mut routines = Vec::new();
    while index < tokens.len() {
        if is_word(&tokens[index], "BEGIN") {
            break;
        }
        if is_routine_keyword(&tokens[index]) {
            if let Some((routine, next)) = fallback_routine(
                document,
                source,
                &tokens,
                index,
                source_unit,
                &package_name,
            ) {
                routines.push(routine);
                index = next;
                continue;
            }
        }
        index += 1;
    }
    Some((source_unit.to_owned(), package_name, routines))
}

fn fallback_routine(
    document: &Document,
    source: &str,
    tokens: &[LexToken],
    keyword_index: usize,
    source_unit: &str,
    package_name: &str,
) -> Option<(DocumentSymbol, usize)> {
    let keyword = tokens.get(keyword_index)?;
    let name = tokens
        .get(keyword_index + 1)
        .filter(|token| token.kind == TokenKind::Word)?;
    let (header_end, definition) = routine_header_end(tokens, keyword_index + 2)?;
    let next = if definition {
        routine_definition_end(tokens, header_end + 1)?
    } else {
        header_end + 1
    };
    let member_kind = if is_word(keyword, "PROCEDURE") {
        "PROCEDURE"
    } else {
        "FUNCTION"
    };
    let role = if definition {
        "IMPLEMENTATION"
    } else {
        "DECLARATION"
    };
    let signature = fallback_routine_signature(source, tokens, keyword_index + 2, header_end);
    let display_signature = source
        .get(keyword.start..tokens[header_end].start)
        .unwrap_or_default()
        .split_whitespace()
        .collect::<Vec<_>>()
        .join(" ");
    let range_end = tokens
        .get(next.saturating_sub(1))
        .map_or(tokens[header_end].end, |token| token.end);
    let start = document.position_at_byte(keyword.start);
    let selection_start = document.position_at_byte(name.start);
    let selection_end = document.position_at_byte(name.end);
    let symbol_id = format!(
        "{}|{}|{}|{}|{}|{}",
        source_unit,
        role,
        member_kind,
        package_name.to_ascii_uppercase(),
        name.text.to_ascii_uppercase(),
        signature
    );
    Some((
        DocumentSymbol {
            symbol_id,
            name: name.text.clone(),
            detail: display_signature.clone(),
            kind: if member_kind == "PROCEDURE" { 6 } else { 12 },
            member_kind: member_kind.to_owned(),
            source_unit: source_unit.to_owned(),
            role: role.to_owned(),
            source_order: start.line,
            signature,
            display_signature,
            visibility: if source_unit == "BODY" {
                "PRIVATE"
            } else {
                "PUBLIC"
            }
            .to_owned(),
            range: Range {
                start,
                end: document.position_at_byte(range_end),
            },
            selection_range: Range {
                start: selection_start,
                end: selection_end,
            },
            children: Vec::new(),
        },
        next,
    ))
}

fn routine_header_end(tokens: &[LexToken], mut index: usize) -> Option<(usize, bool)> {
    let mut parentheses = 0_u32;
    while index < tokens.len() {
        let token = &tokens[index];
        if token.kind == TokenKind::Symbol {
            match token.text.as_str() {
                "(" => parentheses += 1,
                ")" => parentheses = parentheses.saturating_sub(1),
                ";" if parentheses == 0 => return Some((index, false)),
                _ => {}
            }
        } else if parentheses == 0 && (is_word(token, "AS") || is_word(token, "IS")) {
            return Some((index, true));
        }
        index += 1;
    }
    None
}

fn routine_definition_end(tokens: &[LexToken], mut index: usize) -> Option<usize> {
    while index < tokens.len() {
        if is_routine_keyword(&tokens[index]) {
            if let Some((header_end, definition)) = routine_header_end(tokens, index + 2) {
                index = if definition {
                    routine_definition_end(tokens, header_end + 1)?
                } else {
                    header_end + 1
                };
                continue;
            }
        }
        if is_word(&tokens[index], "BEGIN") {
            break;
        }
        index += 1;
    }
    if index == tokens.len() {
        return None;
    }

    let mut blocks = vec!["BEGIN"];
    index += 1;
    while index < tokens.len() {
        let token = &tokens[index];
        if is_word(token, "BEGIN") {
            blocks.push("BEGIN");
        } else if is_word(token, "IF")
            && (index == 0 || !is_word(&tokens[index - 1], "END"))
        {
            blocks.push("IF");
        } else if is_word(token, "LOOP")
            && (index == 0 || !is_word(&tokens[index - 1], "END"))
        {
            blocks.push("LOOP");
        } else if is_word(token, "CASE") {
            blocks.push("CASE");
        } else if is_word(token, "END") {
            let close_kind = tokens.get(index + 1).and_then(|value| {
                ["IF", "LOOP", "CASE"]
                    .into_iter()
                    .find(|kind| is_word(value, kind))
            });
            if let Some(kind) = close_kind {
                if blocks.last() == Some(&kind) {
                    blocks.pop();
                }
                index += 1;
            } else {
                blocks.pop();
            }
            if blocks.is_empty() {
                index += 1;
                if tokens.get(index).is_some_and(|value| value.kind == TokenKind::Word) {
                    index += 1;
                }
                if tokens
                    .get(index)
                    .is_some_and(|value| value.kind == TokenKind::Symbol && value.text == ";")
                {
                    index += 1;
                }
                return Some(index);
            }
        }
        index += 1;
    }
    None
}

fn fallback_routine_signature(
    source: &str,
    tokens: &[LexToken],
    mut index: usize,
    header_end: usize,
) -> String {
    while index < header_end
        && !(tokens[index].kind == TokenKind::Symbol && tokens[index].text == "(")
    {
        index += 1;
    }
    if index == header_end {
        return String::new();
    }
    let mut depth = 1_u32;
    let mut parameter_start = index + 1;
    let mut parameters = Vec::new();
    index += 1;
    while index < header_end {
        let token = &tokens[index];
        if token.kind == TokenKind::Symbol {
            match token.text.as_str() {
                "(" => depth += 1,
                ")" => {
                    depth = depth.saturating_sub(1);
                    if depth == 0 {
                        append_fallback_parameter(
                            source,
                            tokens,
                            parameter_start,
                            index,
                            &mut parameters,
                        );
                        break;
                    }
                }
                "," if depth == 1 => {
                    append_fallback_parameter(
                        source,
                        tokens,
                        parameter_start,
                        index,
                        &mut parameters,
                    );
                    parameter_start = index + 1;
                }
                _ => {}
            }
        }
        index += 1;
    }
    parameters.join(",")
}

fn append_fallback_parameter(
    source: &str,
    tokens: &[LexToken],
    start: usize,
    end: usize,
    parameters: &mut Vec<String>,
) {
    let Some(first) = tokens.get(start) else {
        return;
    };
    let parameter_end = tokens
        .get(end.saturating_sub(1))
        .map_or(first.end, |token| token.end);
    let parameter = normalize_parameter(source.get(first.start..parameter_end).unwrap_or_default());
    if !parameter.is_empty() {
        parameters.push(parameter);
    }
}

fn is_word(token: &LexToken, value: &str) -> bool {
    token.kind == TokenKind::Word && token.text.eq_ignore_ascii_case(value)
}

fn is_routine_keyword(token: &LexToken) -> bool {
    is_word(token, "PROCEDURE") || is_word(token, "FUNCTION")
}

fn symbols_below(document: &Document, source: &str, parent: Node<'_>) -> Vec<DocumentSymbol> {
    let mut symbols = Vec::new();
    let mut cursor = parent.walk();
    for child in parent.named_children(&mut cursor) {
        if let Some(symbol) = symbol_for(document, source, child) {
            symbols.push(symbol);
        } else {
            symbols.extend(symbols_below(document, source, child));
        }
    }
    symbols
}

fn symbol_for(document: &Document, source: &str, node: Node<'_>) -> Option<DocumentSymbol> {
    let (kind, member_kind, role) = match node.kind() {
        "package_definition" => {
            let source_unit = symbol_source_unit(node, source);
            (
                4,
                "PACKAGE",
                if source_unit == "BODY" {
                    "IMPLEMENTATION"
                } else {
                    "DECLARATION"
                },
            )
        }
        "trigger_definition" => (24, "TRIGGER", "IMPLEMENTATION"),
        "program_definition" | "program_declaration" => {
            let prefix = source
                .get(node.start_byte()..node.end_byte())?
                .trim_start()
                .to_ascii_uppercase();
            let role = if node.kind() == "program_definition" {
                "IMPLEMENTATION"
            } else {
                "DECLARATION"
            };
            if prefix.starts_with("PROCEDURE") {
                (6, "PROCEDURE", role)
            } else {
                (12, "FUNCTION", role)
            }
        }
        "type_declaration" => (23, "TYPE", "DECLARATION"),
        "declaration" => {
            let words = tokens(source.get(node.byte_range())?);
            let member_kind = if words.first().is_some_and(|value| is_word(value, "CURSOR")) {
                "CURSOR"
            } else if words.get(1).is_some_and(|value| is_word(value, "CONSTANT")) {
                "CONSTANT"
            } else if words
                .get(1)
                .is_some_and(|value| is_word(value, "EXCEPTION"))
            {
                "EXCEPTION"
            } else {
                "VARIABLE"
            };
            let kind = if member_kind == "CONSTANT" { 14 } else { 13 };
            (kind, member_kind, "DECLARATION")
        }
        "parameter" => (13, "PARAMETER", "DECLARATION"),
        _ => return None,
    };
    let name_node = node.child_by_field_name("name")?;
    let name = source.get(name_node.byte_range())?.trim().to_owned();
    if name.is_empty() {
        return None;
    }
    let signature = if matches!(node.kind(), "program_definition" | "program_declaration") {
        routine_signature(node, source)
    } else {
        String::new()
    };
    let display_signature = if matches!(node.kind(), "program_definition" | "program_declaration") {
        routine_display_signature(node, source)
    } else {
        symbol_detail(
            node.kind(),
            source.get(node.byte_range()).unwrap_or_default(),
        )
    };
    let source_unit = symbol_source_unit(node, source);
    let visibility = symbol_visibility(node, source);
    let symbol_id = format!(
        "{}|{}|{}|{}|{}|{}",
        source_unit,
        role,
        member_kind,
        container_identity(node, source),
        name.to_ascii_uppercase(),
        signature
    );
    Some(DocumentSymbol {
        symbol_id,
        name,
        detail: symbol_detail(
            node.kind(),
            source.get(node.byte_range()).unwrap_or_default(),
        ),
        kind,
        member_kind: member_kind.to_owned(),
        source_unit,
        role: role.to_owned(),
        source_order: node.start_position().row as u32,
        signature,
        display_signature,
        visibility,
        range: Range {
            start: document.position_at_byte(node.start_byte()),
            end: document.position_at_byte(node.end_byte()),
        },
        selection_range: Range {
            start: document.position_at_byte(name_node.start_byte()),
            end: document.position_at_byte(name_node.end_byte()),
        },
        children: symbols_below(document, source, node),
    })
}

fn package_is_body(node: Node<'_>, source: &str) -> bool {
    let prefix_end = node
        .child_by_field_name("name")
        .map_or(node.start_byte(), |name| name.start_byte());
    let prefix = tokens(source.get(node.start_byte()..prefix_end).unwrap_or_default());
    prefix.get(1).is_some_and(|token| is_word(token, "BODY"))
}

fn symbol_source_unit(node: Node<'_>, source: &str) -> String {
    let mut current = Some(node);
    while let Some(value) = current {
        if value.kind() == "package_definition" {
            return if package_is_body(value, source) {
                "BODY".to_owned()
            } else {
                "SPECIFICATION".to_owned()
            };
        }
        current = value.parent();
    }
    "SOURCE".to_owned()
}

fn container_identity(node: Node<'_>, source: &str) -> String {
    let mut parts = Vec::new();
    let mut parent = node.parent();
    while let Some(value) = parent {
        if matches!(
            value.kind(),
            "package_definition" | "program_definition" | "program_declaration"
        ) {
            if let Some(name_node) = value.child_by_field_name("name") {
                let mut part = source
                    .get(name_node.byte_range())
                    .unwrap_or_default()
                    .trim()
                    .to_ascii_uppercase();
                if matches!(value.kind(), "program_definition" | "program_declaration") {
                    part.push('(');
                    part.push_str(&routine_signature(value, source));
                    part.push(')');
                }
                parts.push(part);
            }
        }
        parent = value.parent();
    }
    parts.reverse();
    parts.join("/")
}

fn routine_signature(node: Node<'_>, source: &str) -> String {
    fn collect(node: Node<'_>, source: &str, root: Node<'_>, output: &mut Vec<String>) {
        if node.id() != root.id()
            && matches!(node.kind(), "program_definition" | "program_declaration")
        {
            return;
        }
        if node.kind() == "parameter" {
            if let Some(text) = source.get(node.byte_range()) {
                output.push(normalize_parameter(text));
            }
            return;
        }
        let mut cursor = node.walk();
        for child in node.named_children(&mut cursor) {
            collect(child, source, root, output);
        }
    }

    let mut parameters = Vec::new();
    collect(node, source, node, &mut parameters);
    parameters.join(",")
}

fn routine_display_signature(node: Node<'_>, source: &str) -> String {
    let header = source
        .get(node.byte_range())
        .unwrap_or_default()
        .split_once(';')
        .map_or_else(
            || source.get(node.byte_range()).unwrap_or_default(),
            |(value, _)| value,
        );
    let mut display = header.split_whitespace().collect::<Vec<_>>().join(" ");
    if node.kind() == "program_definition" {
        let upper = display.to_ascii_uppercase();
        if let Some(index) = [" AS ", " IS ", " BEGIN "]
            .iter()
            .filter_map(|marker| upper.find(marker))
            .min()
        {
            display.truncate(index);
        }
    }
    display
}

fn normalize_parameter(source: &str) -> String {
    let mut code = String::new();
    let mut previous_end = 0;
    for token in tokens(source) {
        let raw = &source[token.start..token.end];
        if (token.kind == TokenKind::Symbol && matches!(token.text.as_str(), ":=" | "="))
            || (is_word(&token, "DEFAULT") && !raw.starts_with('"'))
        {
            break;
        }
        if !code.is_empty() && token.start > previous_end {
            code.push(' ');
        }
        code.push_str(raw);
        previous_end = token.end;
    }
    let normalized = code
        .replace(['(', ')', ','], " ")
        .split_whitespace()
        .map(str::to_ascii_uppercase)
        .collect::<Vec<_>>();
    if normalized.len() < 2 {
        return String::new();
    }
    let mut index = 1;
    let mode = if normalized.get(index).is_some_and(|value| value == "OUT") {
        index += 1;
        "OUT"
    } else if normalized.get(index).is_some_and(|value| value == "IN") {
        index += 1;
        if normalized.get(index).is_some_and(|value| value == "OUT") {
            index += 1;
            "IN OUT"
        } else {
            "IN"
        }
    } else {
        "IN"
    };
    let data_type = normalized[index..]
        .iter()
        .take_while(|value| value.as_str() != "DEFAULT")
        .cloned()
        .collect::<Vec<_>>()
        .join(" ");
    format!("{mode}:{data_type}")
}

fn symbol_visibility(node: Node<'_>, source: &str) -> String {
    let mut parent = node.parent();
    let mut inside_program = false;
    while let Some(value) = parent {
        if matches!(value.kind(), "program_definition" | "program_declaration") {
            inside_program = true;
        }
        if value.kind() == "package_definition" {
            if inside_program {
                return "LOCAL".to_owned();
            }
            return if package_is_body(value, source) {
                "PRIVATE".to_owned()
            } else {
                "PUBLIC".to_owned()
            };
        }
        parent = value.parent();
    }
    if inside_program {
        "LOCAL".to_owned()
    } else {
        "PUBLIC".to_owned()
    }
}

fn symbol_detail(node_kind: &str, source: &str) -> String {
    let mut header = source
        .split_once(';')
        .map_or(source, |(value, _)| value)
        .split_whitespace()
        .collect::<Vec<_>>()
        .join(" ");
    if matches!(
        node_kind,
        "package_definition" | "program_definition" | "trigger_definition"
    ) {
        let upper = header.to_ascii_uppercase();
        if let Some(index) = [" AS ", " IS ", " BEGIN "]
            .iter()
            .filter_map(|marker| upper.find(marker))
            .min()
        {
            header.truncate(index);
        }
    }
    header.chars().take(240).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn package_comments_literals_and_initializer_preserve_member_kinds() {
        let source = r#"
CREATE OR REPLACE PACKAGE /* header */ BODY demo_pkg AS
  message_ VARCHAR2(100) := ' CONSTANT PROCEDURE fake_ IS BEGIN END; ';
  limit_ /* variable documentation */ CONSTANT NUMBER := 1;
  invalid_ /* exception documentation */ EXCEPTION;
  -- PROCEDURE comment_only;
  PROCEDURE run_(p_id /* parameter documentation */ IN NUMBER DEFAULT 1) IS
  BEGIN
    NULL;
  END run_;
BEGIN
  message_ := 'END; -- /* initialization string */';
END demo_pkg;
"#;
        let document = Document::open(1, source.to_owned(), None).expect("document");
        assert!(!document.tree.as_ref().expect("tree").root_node().has_error());
        let symbols = document_symbols(&document);
        let package = &symbols[0];
        assert_eq!(package.source_unit, "BODY");
        assert_eq!(package.role, "IMPLEMENTATION");
        for (name, member_kind) in [
            ("message_", "VARIABLE"),
            ("limit_", "CONSTANT"),
            ("invalid_", "EXCEPTION"),
            ("run_", "PROCEDURE"),
        ] {
            let member = package.children.iter().find(|member| member.name == name).expect(name);
            assert_eq!(member.member_kind, member_kind);
            assert_eq!(member.source_unit, "BODY");
            assert_eq!(member.visibility, "PRIVATE");
        }
        assert!(!package.children.iter().any(|member| member.name == "fake_" || member.name == "comment_only"));
        let routine = package.children.iter().find(|member| member.name == "run_").expect("run_");
        assert_eq!(routine.signature, "IN:NUMBER");
        assert_eq!(package.range.end.line, source.lines().count() as u32 - 1);
    }

    #[test]
    fn parses_quoted_package_authid_and_default_parameters() {
        let source = "CREATE PACKAGE \"demo_pkg\" AUTHID CURRENT_USER AS \
            PROCEDURE run_(p_id IN NUMBER DEFAULT 1); END \"demo_pkg\";";
        let document = Document::open(1, source.to_owned(), None).expect("document");
        assert!(!document.tree.as_ref().expect("tree").root_node().has_error());
        let symbols = document_symbols(&document);
        assert_eq!(symbols[0].source_unit, "SPECIFICATION");
        assert_eq!(symbols[0].children[0].signature, "IN:NUMBER");
        assert_eq!(symbols[0].children[0].visibility, "PUBLIC");
        assert_eq!(normalize_parameter("p_id /* docs */ IN /* mode */ NUMBER DEFAULT 1"), "IN:NUMBER");
        assert_eq!(normalize_parameter("p_id IN NUMBER = 1"), "IN:NUMBER");
        assert_eq!(normalize_parameter("p_id IN \"A=B\" DEFAULT 1"), "IN:\"A=B\"");
    }

    #[test]
    fn returns_package_members_and_local_declarations() {
        fn contains(symbols: &[DocumentSymbol], name: &str, kind: u8) -> bool {
            symbols.iter().any(|symbol| {
                symbol.name.eq_ignore_ascii_case(name) && symbol.kind == kind
                    || contains(&symbol.children, name, kind)
            })
        }

        let source = r#"
CREATE OR REPLACE PACKAGE BODY demo_pkg AS
  PROCEDURE save_item(p_id NUMBER) AS
    l_count NUMBER;
    PROCEDURE audit_ IS
    BEGIN
      NULL;
    END;
  BEGIN
    audit_;
  END;
  FUNCTION item_count RETURN NUMBER AS
  BEGIN
    RETURN 1;
  END;
END;
"#;
        let document = Document::open(1, source.to_owned(), None).expect("document");
        let symbols = document_symbols(&document);
        assert_eq!(symbols.len(), 1);
        assert_eq!(symbols[0].name.to_ascii_uppercase(), "DEMO_PKG");
        assert!(contains(&symbols[0].children, "save_item", 6));
        assert!(contains(&symbols[0].children, "audit_", 6));
        assert!(
            contains(&symbols[0].children, "item_count", 12),
            "tree={} symbols={symbols:#?}",
            document.tree.as_ref().expect("tree").root_node().to_sexp()
        );
    }

    #[test]
    fn returns_package_spec_types_cursors_and_parameters() {
        fn contains(symbols: &[DocumentSymbol], name: &str, member_kind: &str) -> bool {
            symbols.iter().any(|symbol| {
                symbol.name.eq_ignore_ascii_case(name) && symbol.member_kind == member_kind
                    || contains(&symbol.children, name, member_kind)
            })
        }

        let source = r#"
CREATE OR REPLACE PACKAGE sales_api AS
  TYPE id_list IS TABLE OF NUMBER;
  default_limit CONSTANT NUMBER := 20;
  current_limit NUMBER := 10;
  CURSOR active_sales IS SELECT id FROM sales_tab;
  invalid_state EXCEPTION;
  PROCEDURE create_sale(id_ IN NUMBER);
  FUNCTION total_(id_ IN NUMBER) RETURN NUMBER;
END sales_api;
"#;
        let document = Document::open(1, source.to_owned(), None).expect("document");
        let symbols = document_symbols(&document);
        assert_eq!(symbols.len(), 1);
        assert_eq!(symbols[0].source_unit, "SPECIFICATION");
        assert_eq!(symbols[0].role, "DECLARATION");
        assert!(contains(&symbols[0].children, "id_list", "TYPE"));
        assert!(contains(&symbols[0].children, "default_limit", "CONSTANT"));
        assert!(contains(&symbols[0].children, "current_limit", "VARIABLE"));
        assert!(contains(&symbols[0].children, "active_sales", "CURSOR"));
        assert!(contains(&symbols[0].children, "invalid_state", "EXCEPTION"));
        assert!(contains(&symbols[0].children, "create_sale", "PROCEDURE"));
        assert!(contains(&symbols[0].children, "total_", "FUNCTION"));
        assert!(contains(&symbols[0].children, "id_", "PARAMETER"));
    }

    #[test]
    fn returns_stable_overload_signatures_and_visibility() {
        let source = r#"
CREATE OR REPLACE PACKAGE demo_api AS
  PROCEDURE run_(value_ IN NUMBER);
  PROCEDURE run_(value_ VARCHAR2);
END demo_api;
"#;
        let document = Document::open(1, source.to_owned(), None).expect("document");
        let symbols = document_symbols(&document);
        let routines = &symbols[0].children;
        assert_eq!(routines[0].signature, "IN:NUMBER");
        assert_eq!(routines[1].signature, "IN:VARCHAR2");
        assert_eq!(routines[0].member_kind, "PROCEDURE");
        assert_eq!(routines[0].source_unit, "SPECIFICATION");
        assert_eq!(routines[0].role, "DECLARATION");
        assert_eq!(
            routines[0].display_signature,
            "PROCEDURE run_(value_ IN NUMBER)"
        );
        assert_eq!(routines[0].visibility, "PUBLIC");
        assert_ne!(routines[0].symbol_id, routines[1].symbol_id);
    }

    #[test]
    fn separates_body_forward_declaration_and_implementation() {
        let source = r#"
CREATE OR REPLACE PACKAGE BODY demo_api AS
  PROCEDURE run_(value_ IN NUMBER);
  PROCEDURE run_(value_ IN NUMBER) AS
  BEGIN
    NULL;
  END;
END demo_api;
"#;
        let document = Document::open(1, source.to_owned(), None).expect("document");
        let symbols = document_symbols(&document);
        let routines = &symbols[0].children;
        assert_eq!(symbols[0].source_unit, "BODY");
        assert_eq!(routines.len(), 2);
        assert_eq!(routines[0].role, "DECLARATION");
        assert_eq!(routines[1].role, "IMPLEMENTATION");
        assert_ne!(routines[0].symbol_id, routines[1].symbol_id);
    }

    #[test]
    fn fallback_keeps_multiline_top_level_routines_and_skips_local_routines() {
        let source = r#"
CREATE OR REPLACE PACKAGE BODY Enterp_Comp_Connect_V190_API AS
  PRAGMA SERIALLY_REUSABLE;
  PROCEDURE Tem_Insert_Data (
    tem_public_rec_ IN Tem_Public_Rec )
  IS
    PROCEDURE Core (
      public_rec_ IN Tem_Public_Rec )
    IS
    BEGIN
      NULL;
    END Core;
  BEGIN
    Core(tem_public_rec_);
  END Tem_Insert_Data;

  PROCEDURE Tem_Insert_Data_Exp (
    tem_public_rec_ IN Tem_Public_Rec )
  IS
  BEGIN
    NULL;
  END Tem_Insert_Data_Exp;
END Enterp_Comp_Connect_V190_API;
"#;
        let document = Document::open(1, source.to_owned(), None).expect("document");
        let (_, _, routines) = fallback_package_routines(&document, source)
            .expect("fallback package members");
        assert_eq!(routines.len(), 2, "routines={routines:#?}");
        assert_eq!(routines[0].name, "Tem_Insert_Data");
        assert_eq!(routines[0].signature, "IN:TEM_PUBLIC_REC");
        assert_eq!(routines[0].role, "IMPLEMENTATION");
        assert_eq!(routines[1].name, "Tem_Insert_Data_Exp");
        assert!(!routines.iter().any(|routine| routine.name == "Core"));

        let symbols = document_symbols(&document);
        let package = symbols
            .iter()
            .find(|symbol| symbol.member_kind == "PACKAGE")
            .expect("package symbol");
        assert!(
            package
                .children
                .iter()
                .any(|routine| routine.name == "Tem_Insert_Data")
        );
        assert!(
            package
                .children
                .iter()
                .any(|routine| routine.name == "Tem_Insert_Data_Exp")
        );
        assert!(!package.children.iter().any(|routine| routine.name == "Core"));
    }
}
