use tree_sitter::Node;

use crate::backward::{LexToken, TokenKind, tokens};
use crate::document::{Document, Position, Range};

/// Resolve declarations in the current document without consulting database metadata.
pub fn definition(document: &Document, position: Position) -> Option<Range> {
    let tree = document.tree.as_ref()?;
    let source = document.source();
    let byte = document.byte_offset(position);
    let resolved = document.position_at_byte(byte);
    if resolved.line != position.line || resolved.character != position.character {
        return None;
    }
    let lexed = tokens(&source);
    let index = lexed
        .iter()
        .position(|token| token.kind == TokenKind::Word && token.start <= byte && byte < token.end)
        .or_else(|| {
            lexed
                .iter()
                .position(|token| token.kind == TokenKind::Word && token.end == byte)
        })?;
    let reference = &lexed[index];
    // A record field or a qualified database member must not resolve to an unrelated local.
    if index > 0 && lexed[index - 1].text == "." {
        return None;
    }
    let name = identifier(&source[reference.start..reference.end]);
    let mut node = tree
        .root_node()
        .descendant_for_byte_range(reference.start, reference.end);
    while let Some(scope) = node {
        if is_scope(scope.kind()) {
            let mut candidates = Vec::new();
            if scope.kind() == "loop_statement" {
                if let Some(declaration) = loop_variable(scope, &lexed, reference.start) {
                    if identifier(&source[declaration.start..declaration.end]) == name {
                        return Some(range(document, declaration.start, declaration.end));
                    }
                }
            } else {
                collect_declarations(
                    scope,
                    scope,
                    &source,
                    reference.start,
                    &name,
                    &mut candidates,
                );
                // Implementations are preferable to forward declarations, but overloads remain ambiguous.
                let implementations: Vec<_> = candidates
                    .iter()
                    .copied()
                    .filter(|(_, implementation)| *implementation)
                    .collect();
                let candidates = if implementations.is_empty() {
                    &candidates
                } else {
                    &implementations
                };
                if candidates.len() == 1 {
                    let target = candidates[0].0;
                    return Some(range(document, target.start_byte(), target.end_byte()));
                }
                if !candidates.is_empty() {
                    return None;
                }
            }
        }
        node = scope.parent();
    }
    None
}

fn identifier(raw: &str) -> String {
    if raw.len() >= 2 && raw.starts_with('"') && raw.ends_with('"') {
        raw[1..raw.len() - 1].replace("\"\"", "\"")
    } else {
        raw.to_uppercase()
    }
}

fn range(document: &Document, start: usize, end: usize) -> Range {
    Range {
        start: document.position_at_byte(start),
        end: document.position_at_byte(end),
    }
}

fn is_scope(kind: &str) -> bool {
    matches!(
        kind,
        "program_definition"
            | "program_declaration"
            | "package_definition"
            | "trigger_definition"
            | "anonymous_block"
            | "loop_statement"
            | "declaration"
    )
}

fn collect_declarations<'a>(
    node: Node<'a>,
    scope: Node<'a>,
    source: &str,
    reference: usize,
    name: &str,
    candidates: &mut Vec<(Node<'a>, bool)>,
) {
    if node.is_error() || node.is_missing() {
        return;
    }
    if matches!(
        node.kind(),
        "declaration"
            | "parameter"
            | "type_declaration"
            | "program_definition"
            | "program_declaration"
            | "package_definition"
            | "trigger_definition"
    ) {
        if let Some(target) = node.child_by_field_name("name") {
            let declared = source.get(target.byte_range()).unwrap_or_default();
            if target.start_byte() <= reference && identifier(declared) == name {
                candidates.push((target, node.kind() == "program_definition"));
            }
        }
        if node.id() != scope.id() {
            return;
        }
    }
    if node.id() != scope.id() && is_scope(node.kind()) {
        return;
    }
    let mut cursor = node.walk();
    for child in node.named_children(&mut cursor) {
        collect_declarations(child, scope, source, reference, name, candidates);
    }
}

fn loop_variable<'a>(
    scope: Node<'_>,
    lexed: &'a [LexToken],
    reference: usize,
) -> Option<&'a LexToken> {
    let start = lexed.partition_point(|token| token.start < scope.start_byte());
    let header = lexed.get(start..)?;
    if !header.first()?.text.eq_ignore_ascii_case("FOR")
        || header.get(1)?.kind != TokenKind::Word
        || !header.get(2)?.text.eq_ignore_ascii_case("IN")
    {
        return None;
    }
    let variable = &header[1];
    let body = header
        .iter()
        .take_while(|token| token.start < scope.end_byte())
        .find(|token| token.text.eq_ignore_ascii_case("LOOP"))?;
    (reference == variable.start || reference >= body.end).then_some(variable)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn target(source: &str, needle: &str) -> Option<usize> {
        let document = Document::open(1, source.to_owned(), None).unwrap();
        let byte = source.rfind(needle).unwrap();
        definition(&document, document.position_at_byte(byte))
            .map(|location| document.byte_offset(location.start))
    }

    #[test]
    fn resolves_record_variable_and_parameters_in_current_routine() {
        let source = "CREATE OR REPLACE PROCEDURE p(p_id IN NUMBER) AS rec_ orders%ROWTYPE; \
            BEGIN use_(rec_.seq_no, p_id); END;";
        assert_eq!(
            target(source, "rec_.seq_no"),
            Some(source.find("rec_ orders").unwrap())
        );
        assert_eq!(
            target(source, "p_id);"),
            Some(source.find("p_id IN").unwrap())
        );
        assert_eq!(target(source, "seq_no"), None);
    }

    #[test]
    fn nested_block_shadows_outer_variable_without_leaking_to_siblings() {
        let source = "DECLARE rec_ NUMBER; BEGIN DECLARE rec_ orders%ROWTYPE; BEGIN use_(rec_.id); END; \
            use_(rec_); END;";
        assert_eq!(
            target(source, "rec_); END;"),
            Some(source.find("rec_ NUMBER").unwrap())
        );
        assert_eq!(
            target(source, "rec_.id"),
            Some(source.find("rec_ orders").unwrap())
        );
        let siblings = "CREATE OR REPLACE PACKAGE BODY p AS \
            PROCEDURE a AS rec_ NUMBER; BEGIN NULL; END; \
            PROCEDURE b AS BEGIN use_(rec_); END; END p;";
        assert_eq!(target(siblings, "rec_);"), None);
    }

    #[test]
    fn resolves_cursor_loop_record_only_inside_its_loop() {
        let source = "DECLARE CURSOR rows_ IS SELECT id FROM orders; BEGIN \
            FOR rec_ IN rows_ LOOP use_(rec_.id); END LOOP; END;";
        assert_eq!(
            target(source, "rec_.id"),
            Some(source.find("rec_ IN").unwrap())
        );
        let outside = source.replace("END LOOP; END;", "END LOOP; use_(rec_); END;");
        assert_eq!(target(&outside, "rec_);"), None);
        let nested = source.replace(
            "use_(rec_.id);",
            "FOR rec_ IN rows_ LOOP use_(rec_.id); END LOOP;",
        );
        assert_eq!(
            target(&nested, "rec_.id"),
            Some(nested.rfind("rec_ IN").unwrap())
        );
    }

    #[test]
    fn does_not_navigate_comments_strings_or_missing_names() {
        let source =
            "DECLARE rec_ NUMBER; BEGIN NULL; -- rec_ comment\n use_('rec_ literal'); END;";
        assert_eq!(target(source, "rec_ comment"), None);
        assert_eq!(target(source, "rec_ literal"), None);
        assert_eq!(target("BEGIN use_(missing_); END;", "missing_"), None);
        assert_eq!(target("DECLARE rec_ NUMBER; BEGIN \"", "\""), None);
    }

    #[test]
    fn quoted_names_preserve_case_and_utf16_positions() {
        let source = "CREATE OR REPLACE PROCEDURE \"record\" AS BEGIN use_('😀', \"record\"); END;";
        assert_eq!(
            target(source, "\"record\");"),
            Some(source.find("\"record\" AS").unwrap())
        );
        assert_eq!(
            target(&source.replace("\"record\");", "record);"), "record);"),
            None
        );
        let source = "CREATE OR REPLACE PROCEDURE p AS text_ VARCHAR2(20) := '😀'; \
            rec_ NUMBER; BEGIN use_(rec_); END;";
        assert_eq!(
            target(source, "rec_);"),
            Some(source.find("rec_ NUMBER").unwrap())
        );
    }

    #[test]
    fn resolves_constants_and_cursor_declarations() {
        let source = "DECLARE limit_ CONSTANT NUMBER := 1; \
            CURSOR rows_ IS SELECT id FROM orders; BEGIN use_(limit_); OPEN rows_; END;";
        assert_eq!(
            target(source, "limit_);"),
            Some(source.find("limit_ CONSTANT").unwrap())
        );
        assert_eq!(
            target(source, "rows_;"),
            Some(source.find("rows_ IS").unwrap())
        );
    }

    #[test]
    fn does_not_guess_between_overloads() {
        let source = "CREATE OR REPLACE PACKAGE BODY p AS \
            PROCEDURE f(x IN NUMBER) AS BEGIN NULL; END; \
            PROCEDURE f(x IN VARCHAR2) AS BEGIN NULL; END; \
            PROCEDURE g AS BEGIN f(1); END; END p;";
        assert_eq!(target(source, "f(1)"), None);
    }

    #[test]
    fn nested_record_declaration_survives_ordered_aggregate_cursor() {
        let source = r#"CREATE OR REPLACE PACKAGE BODY sample_api AS
TYPE Public_Rec IS RECORD (use_condition VARCHAR2(5));
PROCEDURE outer_(lu_name_ VARCHAR2, key_ref_ VARCHAR2) IS
    PROCEDURE inner_(lu_name_ VARCHAR2, key_ref_ VARCHAR2) IS
        rec_con_ Public_Rec;
        sql_ VARCHAR2(30000);
        CURSOR aggregate_rows_ IS
            SELECT LISTAGG(DECODE(is_valid, 'TRUE', condition_, '0'), ',')
                       WITHIN GROUP (ORDER BY line_no ASC) AS condition_text
              FROM sample_rows
             WHERE lu_name = lu_name_;
        CURSOR condition_rows_(condition_ VARCHAR2) IS
            SELECT LISTAGG(CASE WHEN INSTR(condition_, line_no) > 0 THEN 1 ELSE 0 END, ',')
                       WITHIN GROUP (ORDER BY line_no DESC)
              FROM sample_rows;
    BEGIN
        rec_con_ = "Get"(key_ref_);
        IF rec_con_.use_condition = 'FALSE' OR rec_con_.use_condition IS NULL THEN
            RETURN;
        END IF;
    END inner_;
BEGIN
    inner_(lu_name_, key_ref_);
END outer_;
END sample_api;"#;
        let document = Document::open(1, source.to_owned(), None).unwrap();
        let root = document.tree.as_ref().unwrap().root_node();
        assert!(!root.has_error(), "{}", root.to_sexp());
        let declaration = source.find("rec_con_ Public_Rec").unwrap();
        assert_eq!(target(source, "rec_con_ ="), Some(declaration));
        assert_eq!(target(source, "rec_con_.use_condition"), Some(declaration));
        assert_eq!(target(source, "use_condition IS"), None);
        assert_eq!(
            target(source, "lu_name_;"),
            Some(source.find("inner_(lu_name_").unwrap() + "inner_(".len())
        );
    }

    #[test]
    fn nested_routine_shadowing_stays_local_with_ordered_aggregates() {
        let source = "CREATE OR REPLACE PACKAGE BODY p AS \
            PROCEDURE outer_ IS rec_con_ Public_Rec; \
                PROCEDURE inner_ IS rec_con_ Public_Rec; \
                    CURSOR rows_ IS SELECT LISTAGG(value_, ',') WITHIN GROUP (ORDER BY id) FROM items; \
                BEGIN use_(rec_con_); END inner_; \
                PROCEDURE sibling_ IS BEGIN use_(rec_con_); END sibling_; \
            BEGIN use_(rec_con_); END outer_; END p;";
        let outer = source.find("rec_con_ Public_Rec").unwrap();
        let inner = source.rfind("rec_con_ Public_Rec").unwrap();
        assert_eq!(target(source, "rec_con_); END inner_;"), Some(inner));
        assert_eq!(target(source, "rec_con_); END sibling_;"), Some(outer));
        assert_eq!(target(source, "rec_con_); END outer_;"), Some(outer));
        let only_in_inner = source.replacen("rec_con_ Public_Rec;", "", 1);
        assert_eq!(target(&only_in_inner, "rec_con_); END sibling_;"), None);
        assert_eq!(target(&only_in_inner, "rec_con_); END outer_;"), None);
    }

    #[test]
    fn returns_no_definition_when_tree_is_unavailable() {
        let mut document =
            Document::open(1, "DECLARE x NUMBER; BEGIN use_(x); END;".into(), None).unwrap();
        document.tree = None;
        assert!(
            definition(
                &document,
                Position {
                    line: 0,
                    character: 36
                }
            )
            .is_none()
        );
    }
}
