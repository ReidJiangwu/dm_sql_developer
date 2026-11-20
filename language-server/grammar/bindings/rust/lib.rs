// DM8 SQL and DMSQL grammar for Tree-sitter.

use tree_sitter_language::LanguageFn;

unsafe extern "C" {
    fn tree_sitter_dm_sql() -> *const ();
}

/// Returns the Tree-sitter language function for DM SQL.
pub const LANGUAGE: LanguageFn = unsafe { LanguageFn::from_raw(tree_sitter_dm_sql) };

#[cfg(test)]
mod tests {
    #[test]
    fn ordered_aggregate_calls_keep_nested_expressions_and_window_suffixes() {
        let mut parser = tree_sitter::Parser::new();
        parser.set_language(&super::LANGUAGE.into()).unwrap();
        for sql in [
            "SELECT LISTAGG(name, ',') WITHIN GROUP (ORDER BY seq ASC, id DESC NULLS LAST) FROM items;",
            "SELECT LISTAGG(DECODE(valid_, 'TRUE', value_, '0'), ',') WITHIN GROUP (ORDER BY seq) FROM items;",
            "SELECT LISTAGG(CASE WHEN id > 0 THEN 1 ELSE 0 END, ',') WITHIN GROUP (ORDER BY seq) FROM items;",
            "SELECT LISTAGG(name, ',') WITHIN GROUP (ORDER BY seq) OVER (PARTITION BY group_) FROM items;",
            "SELECT fn_(LISTAGG(name, ',') WITHIN GROUP (ORDER BY seq)) FROM items;",
        ] {
            let tree = parser.parse(sql, None).unwrap();
            assert!(
                !tree.root_node().has_error(),
                "{}",
                tree.root_node().to_sexp()
            );
        }
    }

    #[test]
    fn grammar_loads() {
        let mut parser = tree_sitter::Parser::new();
        parser
            .set_language(&super::LANGUAGE.into())
            .expect("DM SQL grammar must load");
        for sql in [
            "SELECT 1 FROM DUAL;",
            "SELECT o.ORDER_ID, o.AMOUNT FROM ORDERS o WHERE o.AMOUNT > 10;",
            "WITH recent AS (SELECT o.ORDER_ID, o.AMOUNT FROM ORDERS o) SELECT r.ORDER_ID FROM recent r;",
        ] {
            let tree = parser
                .parse(sql, None)
                .expect("parser must return a syntax tree");
            assert!(
                !tree.root_node().has_error(),
                "{}",
                tree.root_node().to_sexp()
            );
        }
    }
}
