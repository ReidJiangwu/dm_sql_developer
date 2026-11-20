use std::collections::HashMap;

use crate::backward::{LexToken, TokenKind, tokens};

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct QueryScope {
    pub start: usize,
    pub end: usize,
    pub depth: usize,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct RelationSymbol {
    pub schema: Option<String>,
    pub name: String,
    pub alias: String,
    pub depth: usize,
    pub declaration: usize,
    pub projected: Option<Vec<String>>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CommonTableExpression {
    pub name: String,
    pub columns: Vec<String>,
    pub depth: usize,
    pub declaration: usize,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct LocalSymbol {
    pub name: String,
    pub data_type: String,
    pub depth: usize,
    pub declaration: usize,
}

#[derive(Clone, Debug, Default)]
pub struct SemanticSnapshot {
    pub query_scopes: Vec<QueryScope>,
    pub relations: Vec<RelationSymbol>,
    pub ctes: Vec<CommonTableExpression>,
    pub locals: Vec<LocalSymbol>,
    pub has_parse_errors: bool,
}

impl SemanticSnapshot {
    pub fn build(text: &str, has_parse_errors: bool) -> Self {
        let tokens = tokens(text);
        let depths = token_depths(&tokens);
        Self {
            query_scopes: extract_query_scopes(text.len(), &tokens, &depths),
            relations: extract_relations(text, &tokens, &depths),
            ctes: extract_ctes(&tokens, &depths),
            locals: extract_locals(&tokens, &depths),
            has_parse_errors,
        }
    }

    pub fn cursor_depth(&self, cursor: usize) -> usize {
        self.query_scopes
            .iter()
            .filter(|scope| scope.start <= cursor && cursor <= scope.end)
            .map(|scope| scope.depth)
            .max()
            .unwrap_or(0)
    }

    pub fn resolve_alias(&self, qualifier: &str, cursor: usize) -> Option<&RelationSymbol> {
        let matches: Vec<_> = self.relations
            .iter()
            .filter(|relation| {
                relation.alias.eq_ignore_ascii_case(qualifier)
                    && self.relation_visible(relation, cursor)
            })
            .collect();
        let depth = matches.iter().map(|relation| relation.depth).max()?;
        let mut nearest = matches.into_iter().filter(|relation| relation.depth == depth);
        let first = nearest.next()?;
        if nearest.next().is_some() { None } else { Some(first) }
    }

    pub fn resolve_cte(&self, name: &str, cursor: usize) -> Option<&CommonTableExpression> {
        let cursor_depth = self.cursor_depth(cursor);
        self.ctes
            .iter()
            .filter(|cte| {
                cte.name.eq_ignore_ascii_case(name)
                    && cte.depth <= cursor_depth
                    && cte.declaration <= cursor
            })
            .max_by_key(|cte| (cte.depth, cte.declaration))
    }

    pub fn visible_relations(&self, cursor: usize) -> impl Iterator<Item = &RelationSymbol> {
        self.relations
            .iter()
            .filter(move |relation| self.relation_visible(relation, cursor))
    }

    pub fn visible_locals(&self, cursor: usize) -> impl Iterator<Item = &LocalSymbol> {
        let cursor_depth = self.cursor_depth(cursor);
        self.locals
            .iter()
            .filter(move |local| local.depth <= cursor_depth && local.declaration <= cursor)
    }

    fn relation_visible(&self, relation: &RelationSymbol, cursor: usize) -> bool {
        let cursor_depth = self.cursor_depth(cursor);
        if relation.depth > cursor_depth {
            return false;
        }
        let matching_scopes = self
            .query_scopes
            .iter()
            .filter(|scope| {
                scope.depth == relation.depth && scope.start <= cursor && cursor <= scope.end
            })
            .collect::<Vec<_>>();
        if matching_scopes.is_empty() {
            return relation.declaration <= cursor;
        }
        matching_scopes
            .into_iter()
            .any(|scope| scope.start <= relation.declaration && relation.declaration <= scope.end)
    }
}

fn token_depths(tokens: &[LexToken]) -> Vec<usize> {
    let mut depth = 0_usize;
    let mut result = Vec::with_capacity(tokens.len());
    for token in tokens {
        if token.kind == TokenKind::Symbol && token.text == ")" {
            depth = depth.saturating_sub(1);
        }
        result.push(depth);
        if token.kind == TokenKind::Symbol && token.text == "(" {
            depth = depth.saturating_add(1);
        }
    }
    result
}

fn extract_query_scopes(text_len: usize, tokens: &[LexToken], depths: &[usize]) -> Vec<QueryScope> {
    let mut scopes = Vec::new();
    for (index, token) in tokens.iter().enumerate() {
        if !is_word(token, "SELECT") {
            continue;
        }
        let depth = depths[index];
        let end = tokens[index + 1..]
            .iter()
            .zip(&depths[index + 1..])
            .find_map(|(candidate, candidate_depth)| {
                (*candidate_depth < depth
                    || (*candidate_depth == depth
                        && ((candidate.kind == TokenKind::Symbol && candidate.text == ";")
                            || matches_word(candidate, &["UNION", "MINUS", "INTERSECT"]))))
                .then_some(candidate.start)
            })
            .unwrap_or(text_len);
        scopes.push(QueryScope {
            start: token.start,
            end,
            depth,
        });
    }
    scopes
}

fn extract_relations(source: &str, tokens: &[LexToken], depths: &[usize]) -> Vec<RelationSymbol> {
    let mut relations = Vec::new();
    let mut index = 0;
    while index < tokens.len() {
        if !matches_word(&tokens[index], &["FROM", "JOIN", "UPDATE", "INTO", "USING"]) {
            index += 1;
            continue;
        }
        let declaration_depth = depths[index];
        index += 1;
        if tokens.get(index).is_some_and(|token| token.text == "(") {
            let open = index;
            if let Some(close) = (open + 1..tokens.len()).find(|&i| tokens[i].text == ")" && depths[i] == declaration_depth) {
                let mut alias_index = close + 1;
                if tokens.get(alias_index).is_some_and(|t| is_word(t, "AS")) { alias_index += 1; }
                if let Some(alias) = tokens.get(alias_index).filter(|t| t.kind == TokenKind::Word && !is_reserved(&t.text)) {
                    relations.push(RelationSymbol {
                        schema: None, name: alias.text.clone(), alias: alias.text.clone(),
                        depth: declaration_depth, declaration: tokens[open].start,
                        projected: Some(projected_columns(&tokens[open + 1..close], &depths[open + 1..close])),
                    });
                }
            }
            index += 1;
            continue;
        }
        let Some(first) = tokens
            .get(index)
            .filter(|token| token.kind == TokenKind::Word)
        else {
            continue;
        };
        let declaration = first.start;
        let normalized = |token: &LexToken| if source.as_bytes().get(token.start) == Some(&b'"') {
            token.text.clone()
        } else { token.text.to_ascii_uppercase() };
        let mut parts = vec![normalized(first)];
        index += 1;
        while index + 1 < tokens.len()
            && tokens[index].kind == TokenKind::Symbol
            && tokens[index].text == "."
            && tokens[index + 1].kind == TokenKind::Word
        {
            parts.push(normalized(&tokens[index + 1]));
            index += 2;
        }

        if tokens.get(index).is_some_and(|token| is_word(token, "AS")) {
            index += 1;
        }
        let alias = tokens
            .get(index)
            .filter(|token| token.kind == TokenKind::Word && !is_reserved(&token.text))
            .map(|token| token.text.clone())
            .unwrap_or_else(|| parts.last().cloned().unwrap_or_default());
        let (schema, name) = if parts.len() > 1 {
            (
                Some(parts[..parts.len() - 1].join(".")),
                parts.last().cloned().unwrap_or_default(),
            )
        } else {
            (None, parts[0].clone())
        };
        relations.push(RelationSymbol {
            schema,
            name,
            alias,
            depth: declaration_depth,
            declaration,
            projected: None,
        });
    }
    relations
}

fn extract_ctes(tokens: &[LexToken], depths: &[usize]) -> Vec<CommonTableExpression> {
    let mut ctes = Vec::new();
    let mut index = 0;
    while index + 3 < tokens.len() {
        if !is_word(&tokens[index], "WITH") {
            index += 1;
            continue;
        }
        let cte_depth = depths[index];
        index += 1;
        if tokens
            .get(index)
            .is_some_and(|token| is_word(token, "RECURSIVE"))
        {
            index += 1;
        }
        loop {
            let Some(name_token) = tokens
                .get(index)
                .filter(|token| token.kind == TokenKind::Word)
            else {
                break;
            };
            let cte_name = name_token.text.clone();
            let declaration = name_token.start;
            index += 1;

            let mut explicit_columns = Vec::new();
            if tokens.get(index).is_some_and(|token| token.text == "(")
                && !tokens
                    .get(index + 1)
                    .is_some_and(|token| is_word(token, "SELECT"))
            {
                index += 1;
                while index < tokens.len() && tokens[index].text != ")" {
                    if tokens[index].kind == TokenKind::Word {
                        explicit_columns.push(tokens[index].text.clone());
                    }
                    index += 1;
                }
                index += usize::from(index < tokens.len());
            }
            if !tokens.get(index).is_some_and(|token| is_word(token, "AS")) {
                break;
            }
            index += 1;
            if !tokens.get(index).is_some_and(|token| token.text == "(") {
                break;
            }
            let query_open_depth = depths[index];
            index += 1;
            let query_start = index;
            while index < tokens.len()
                && !(tokens[index].text == ")" && depths[index] == query_open_depth)
            {
                index += 1;
            }
            let query_end = index;
            let columns = if explicit_columns.is_empty() {
                projected_columns(
                    &tokens[query_start..query_end],
                    &depths[query_start..query_end],
                )
            } else {
                explicit_columns
            };
            ctes.push(CommonTableExpression {
                name: cte_name,
                columns,
                depth: cte_depth,
                declaration,
            });
            index += usize::from(index < tokens.len());
            if !tokens.get(index).is_some_and(|token| token.text == ",") {
                break;
            }
            index += 1;
        }
    }
    ctes
}

fn projected_columns(tokens: &[LexToken], depths: &[usize]) -> Vec<String> {
    let Some(select_index) = tokens.iter().position(|token| is_word(token, "SELECT")) else {
        return Vec::new();
    };
    let select_depth = depths[select_index];
    let end = tokens
        .iter()
        .enumerate()
        .skip(select_index + 1)
        .find_map(|(index, token)| {
            (depths[index] == select_depth && is_word(token, "FROM")).then_some(index)
        })
        .unwrap_or(tokens.len());

    let mut columns = Vec::new();
    let mut item_start = select_index + 1;
    for item_end in (select_index + 1..=end).filter(|index| {
        *index == end
            || (tokens[*index].kind == TokenKind::Symbol
                && tokens[*index].text == ","
                && depths[*index] == select_depth)
    }) {
        let item = &tokens[item_start..item_end];
        let alias = item
            .iter()
            .enumerate()
            .rev()
            .find_map(|(index, token)| {
                (index > 0 && depths[item_start + index] == select_depth
                    && token.kind == TokenKind::Word && is_word(&item[index - 1], "AS"))
                    .then(|| token.text.clone())
            })
            .or_else(|| {
                let last = item.last().filter(|t| t.kind == TokenKind::Word && !is_reserved(&t.text))?;
                let simple_column = item.iter().enumerate().all(|(i, t)| {
                    if i % 2 == 0 { t.kind == TokenKind::Word } else { t.text == "." }
                });
                let alias = item.len() > 1 && item.get(item.len() - 2).is_some_and(|t| {
                    t.kind == TokenKind::Word || matches!(t.text.as_str(), ")" | "'")
                        || t.kind == TokenKind::Number
                });
                (simple_column || alias).then(|| last.text.clone())
            });
        if let Some(alias) = alias {
            columns.push(alias);
        }
        item_start = item_end.saturating_add(1);
    }
    columns
}

fn extract_locals(tokens: &[LexToken], depths: &[usize]) -> Vec<LocalSymbol> {
    let mut locals = Vec::new();
    let mut active_declarations: HashMap<usize, bool> = HashMap::new();
    let mut index = 0;
    while index < tokens.len() {
        let depth = depths[index];
        if is_word(&tokens[index], "DECLARE") {
            active_declarations.insert(depth, true);
            index += 1;
            continue;
        }
        if is_word(&tokens[index], "BEGIN") {
            active_declarations.remove(&depth);
            index += 1;
            continue;
        }
        if active_declarations.get(&depth).copied().unwrap_or(false)
            && tokens[index].kind == TokenKind::Word
            && !is_reserved(&tokens[index].text)
            && tokens
                .get(index + 1)
                .is_some_and(|token| token.kind == TokenKind::Word)
        {
            let name = tokens[index].text.clone();
            let data_type = tokens[index + 1].text.clone();
            locals.push(LocalSymbol {
                name,
                data_type,
                depth,
                declaration: tokens[index].start,
            });
            while index < tokens.len() && tokens[index].text != ";" {
                index += 1;
            }
        }
        index += 1;
    }
    locals
}

fn is_word(token: &LexToken, expected: &str) -> bool {
    token.kind == TokenKind::Word && token.text.eq_ignore_ascii_case(expected)
}

fn matches_word(token: &LexToken, expected: &[&str]) -> bool {
    expected.iter().any(|word| is_word(token, word))
}

fn is_reserved(word: &str) -> bool {
    matches!(
        word.to_ascii_uppercase().as_str(),
        "ALL"
            | "ALTER"
            | "AND"
            | "AS"
            | "ASC"
            | "BEGIN"
            | "BY"
            | "CASE"
            | "CONNECT"
            | "CREATE"
            | "DECLARE"
            | "DELETE"
            | "DESC"
            | "DISTINCT"
            | "DROP"
            | "ELSE"
            | "END"
            | "EXCEPTION"
            | "FROM"
            | "FULL"
            | "FUNCTION"
            | "GROUP"
            | "HAVING"
            | "IF"
            | "IN"
            | "INNER"
            | "INSERT"
            | "INTO"
            | "IS"
            | "JOIN"
            | "LEFT"
            | "LOOP"
            | "MERGE"
            | "NOT"
            | "NULL"
            | "ON"
            | "OR"
            | "ORDER"
            | "OUTER"
            | "PACKAGE"
            | "PROCEDURE"
            | "RETURN"
            | "RIGHT"
            | "SELECT"
            | "SET"
            | "THEN"
            | "TRIGGER"
            | "UNION"
            | "UPDATE"
            | "USING"
            | "VALUES"
            | "VIEW"
            | "WHEN"
            | "WHERE"
            | "WHILE"
            | "WITH"
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn extracts_aliases_ctes_and_locals() {
        let sql = "WITH recent AS (SELECT o.ORDER_ID, o.AMOUNT FROM ORDERS o) \
                   SELECT r.ORDER_ID FROM recent r; \
                   DECLARE total NUMBER; BEGIN total := 1; END;";
        let semantic = SemanticSnapshot::build(sql, false);

        assert!(
            semantic
                .relations
                .iter()
                .any(|relation| relation.alias == "r")
        );
        assert_eq!(
            semantic
                .resolve_cte("recent", sql.find("r.ORDER_ID").unwrap())
                .unwrap()
                .columns,
            vec!["ORDER_ID", "AMOUNT"]
        );
        assert!(semantic.locals.iter().any(|local| local.name == "total"));
    }

    #[test]
    fn resolves_forward_alias_only_inside_its_query_scope() {
        let sql = "SELECT o. FROM ORDERS o; SELECT o. FROM OTHER_ORDERS o;";
        let semantic = SemanticSnapshot::build(sql, true);
        let first_cursor = sql.find("o.").unwrap() + 2;
        let second_cursor = sql.rfind("o.").unwrap() + 2;

        assert_eq!(
            semantic.resolve_alias("o", first_cursor).unwrap().name,
            "ORDERS"
        );
        assert_eq!(
            semantic.resolve_alias("o", second_cursor).unwrap().name,
            "OTHER_ORDERS"
        );
    }

    #[test]
    fn resolves_outer_forward_alias_inside_correlated_subquery() {
        let sql = "SELECT (SELECT c. FROM ORDERS o) FROM CUSTOMERS c";
        let semantic = SemanticSnapshot::build(sql, true);
        let cursor = sql.find("c.").unwrap() + 2;

        assert_eq!(
            semantic.resolve_alias("c", cursor).unwrap().name,
            "CUSTOMERS"
        );
    }
}
