const PREC = {
  OR: 1,
  AND: 2,
  NOT: 3,
  COMPARE: 4,
  CONCAT: 5,
  ADD: 6,
  MULTIPLY: 7,
  UNARY: 8,
  CALL: 9,
  MEMBER: 10,
};

module.exports = grammar({
  name: "dm_sql",

  extras: ($) => [
    /\s/,
    $.line_comment,
    $.block_comment,
  ],

  conflicts: ($) => [
    [$.relation_reference, $.expression],
    [$.qualified_name, $.member_expression],
  ],

  rules: {
    source_file: ($) => repeat(choice(
      $.statement,
      $.slash_separator,
      $.empty_statement,
    )),

    statement: ($) => prec.right(seq(
      choice(
        $.select_statement,
        $.insert_statement,
        $.update_statement,
        $.delete_statement,
        $.merge_statement,
        $.create_statement,
        $.alter_statement,
        $.drop_statement,
        $.anonymous_block,
        $.assignment_statement,
        $.call_statement,
        $.open_statement,
        $.fetch_statement,
        $.close_statement,
        $.execute_immediate_statement,
        $.null_statement,
        $.raise_statement,
        $.return_statement,
        $.if_statement,
        $.loop_statement,
      ),
      optional(";"),
    )),

    empty_statement: () => ";",
    slash_separator: () => token(seq("/", /\r?\n/)),

    select_statement: ($) => prec.right(seq(
      optional($.with_clause),
      $.select_core,
      repeat($.set_operation),
      optional($.order_by_clause),
      optional($.limit_clause),
      optional($.for_update_clause),
    )),

    with_clause: ($) => seq(
      kw("WITH"),
      optional(kw("RECURSIVE")),
      commaSep1($.common_table_expression),
    ),

    common_table_expression: ($) => seq(
      field("name", $.identifier),
      optional(seq("(", commaSep1($.identifier), ")")),
      kw("AS"),
      "(",
      field("query", $.select_statement),
      ")",
    ),

    select_core: ($) => prec.right(seq(
      kw("SELECT"),
      optional(seq(
        kw("TOP"),
        choice($.number, seq("(", $.number, ")")),
        optional(kw("PERCENT")),
        optional(seq(kw("WITH"), kw("TIES"))),
      )),
      optional(choice(kw("ALL"), kw("DISTINCT"))),
      commaSep1($.select_item),
      optional($.into_clause),
      optional($.from_clause),
      optional($.where_clause),
      optional($.connect_by_clause),
      optional($.group_by_clause),
      optional($.having_clause),
    )),

    select_item: ($) => prec.right(seq(
      choice(
        "*",
        seq(
          choice($.identifier, $.quoted_identifier),
          repeat(seq(".", choice($.identifier, $.quoted_identifier))),
          ".",
          "*",
        ),
        $.expression,
      ),
      optional(choice(
        seq(kw("AS"), field("alias", $.identifier)),
        field("alias", $.identifier),
      )),
    )),

    from_clause: ($) => prec.right(seq(
      kw("FROM"),
      commaSep1($.table_expression),
    )),

    into_clause: ($) => seq(
      optional(kw("BULK")),
      kw("INTO"),
      commaSep1($.qualified_name),
    ),

    table_expression: ($) => seq(
      $.relation_reference,
      repeat($.join_clause),
      optional(choice($.pivot_clause, $.unpivot_clause)),
    ),

    relation_reference: ($) => prec.right(seq(
      choice(
        field("name", $.qualified_name),
        $.table_function,
        seq("(", field("query", $.select_statement), ")"),
      ),
      optional(seq(
        choice(kw("PARTITION"), kw("SUBPARTITION")),
        optional(kw("FOR")),
        "(",
        commaSep1($.expression),
        ")",
      )),
      optional(choice(
        seq(kw("AS"), field("alias", $.identifier)),
        field("alias", $.identifier),
      )),
    )),

    table_function: ($) => seq(kw("TABLE"), "(", $.expression, ")"),

    join_clause: ($) => prec.right(seq(
      optional(choice(
        kw("NATURAL"),
        kw("INNER"),
        kw("LEFT"),
        kw("RIGHT"),
        kw("FULL"),
        kw("CROSS"),
        seq(kw("LEFT"), kw("OUTER")),
        seq(kw("RIGHT"), kw("OUTER")),
        seq(kw("FULL"), kw("OUTER")),
      )),
      kw("JOIN"),
      $.relation_reference,
      optional(choice(
        seq(kw("ON"), $.expression),
        seq(kw("USING"), "(", commaSep1($.identifier), ")"),
      )),
    )),

    where_clause: ($) => seq(kw("WHERE"), $.expression),
    connect_by_clause: ($) => seq(
      optional(seq(kw("START"), kw("WITH"), $.expression)),
      kw("CONNECT"),
      kw("BY"),
      optional(kw("NOCYCLE")),
      $.expression,
    ),
    group_by_clause: ($) => prec.right(seq(kw("GROUP"), kw("BY"), commaSep1($.expression))),
    having_clause: ($) => seq(kw("HAVING"), $.expression),
    order_by_clause: ($) => prec.right(seq(
      kw("ORDER"),
      kw("BY"),
      commaSep1(seq(
        $.expression,
        optional(choice(kw("ASC"), kw("DESC"))),
        optional(seq(kw("NULLS"), choice(kw("FIRST"), kw("LAST")))),
      )),
    )),
    limit_clause: ($) => prec.right(choice(
      seq(kw("LIMIT"), $.expression, optional(seq(kw("OFFSET"), $.expression))),
      seq(kw("OFFSET"), $.expression, kw("ROWS"), optional(fetchRows($))),
      fetchRows($),
    )),
    for_update_clause: ($) => prec.right(seq(kw("FOR"), kw("UPDATE"), optional(seq(kw("OF"), commaSep1($.qualified_name))))),
    set_operation: ($) => seq(
      choice(kw("UNION"), kw("INTERSECT"), kw("MINUS"), kw("EXCEPT")),
      optional(choice(kw("ALL"), kw("DISTINCT"))),
      optional(seq(kw("CORRESPONDING"), optional(seq(kw("BY"), "(", commaSep1($.identifier), ")")))),
      $.select_core,
    ),

    pivot_clause: ($) => seq(
      kw("PIVOT"),
      optional(seq(kw("XML"))),
      "(",
      commaSep1($.expression),
      kw("FOR"),
      commaSep1($.qualified_name),
      kw("IN"),
      "(",
      commaSep1($.expression),
      ")",
      ")",
    ),

    unpivot_clause: ($) => seq(
      kw("UNPIVOT"),
      optional(seq(optional(kw("INCLUDE")), kw("NULLS"))),
      "(",
      commaSep1($.qualified_name),
      kw("FOR"),
      commaSep1($.qualified_name),
      kw("IN"),
      "(",
      commaSep1($.expression),
      ")",
      ")",
    ),

    insert_statement: ($) => prec.right(seq(
      kw("INSERT"),
      kw("INTO"),
      field("target", $.qualified_name),
      optional(seq("(", commaSep1($.identifier), ")")),
      choice(
        seq(kw("VALUES"), commaSep1(seq("(", optional(commaSep1($.expression)), ")"))),
        $.select_statement,
      ),
    )),

    update_statement: ($) => prec.right(seq(
      kw("UPDATE"),
      field("target", $.relation_reference),
      kw("SET"),
      commaSep1(seq($.qualified_name, "=", $.expression)),
      optional($.where_clause),
      optional($.returning_clause),
    )),

    delete_statement: ($) => seq(
      kw("DELETE"),
      kw("FROM"),
      field("target", $.relation_reference),
      optional($.where_clause),
      optional($.returning_clause),
    ),

    merge_statement: ($) => prec.right(seq(
      kw("MERGE"),
      kw("INTO"),
      field("target", $.relation_reference),
      kw("USING"),
      field("source", $.relation_reference),
      kw("ON"),
      "(",
      $.expression,
      ")",
      repeat1(choice($.merge_update_clause, $.merge_insert_clause)),
    )),
    merge_update_clause: ($) => prec.right(seq(
      kw("WHEN"), kw("MATCHED"), kw("THEN"), kw("UPDATE"), kw("SET"),
      commaSep1(seq($.qualified_name, "=", $.expression)),
      optional($.where_clause),
    )),
    merge_insert_clause: ($) => prec.right(seq(
      kw("WHEN"), kw("NOT"), kw("MATCHED"), kw("THEN"), kw("INSERT"),
      optional(seq("(", commaSep1($.identifier), ")")),
      kw("VALUES"), "(", commaSep1($.expression), ")",
      optional($.where_clause),
    )),

    returning_clause: ($) => seq(
      kw("RETURNING"),
      commaSep1($.expression),
      kw("INTO"),
      commaSep1($.qualified_name),
    ),

    create_statement: ($) => prec.right(seq(
      kw("CREATE"),
      optional(seq(kw("OR"), kw("REPLACE"))),
      choice(
        seq(choice(kw("TABLE"), kw("VIEW"), kw("INDEX"), kw("SEQUENCE"), kw("SYNONYM")), $.qualified_name, repeat($.ddl_fragment)),
        $.program_definition,
        $.package_definition,
        $.trigger_definition,
      ),
    )),
    alter_statement: ($) => prec.right(seq(kw("ALTER"), $.object_kind, $.qualified_name, repeat($.ddl_fragment))),
    drop_statement: ($) => prec.right(seq(kw("DROP"), $.object_kind, $.qualified_name, repeat($.ddl_fragment))),
    object_kind: () => choice(
      kw("TABLE"), kw("VIEW"), kw("INDEX"), kw("SEQUENCE"), kw("SYNONYM"),
      kw("PROCEDURE"), kw("FUNCTION"), kw("PACKAGE"), kw("TRIGGER"), kw("TYPE"),
    ),
    ddl_fragment: ($) => choice($.identifier, $.number, $.string, $.quoted_identifier, $.operator, "(", ")", ",", "."),

    program_definition: ($) => prec.right(seq(
      choice(kw("PROCEDURE"), kw("FUNCTION")),
      field("name", $.qualified_name),
      optional($.parameter_list),
      optional(seq(kw("RETURN"), $.type_name)),
      choice(kw("AS"), kw("IS")),
      repeat(choice(
        $.declaration,
        $.type_declaration,
        $.program_declaration,
        $.program_definition,
      )),
      $.block_body,
      optional(";"),
    )),
    package_definition: ($) => prec.right(seq(
      kw("PACKAGE"),
      optional(kw("BODY")),
      field("name", $.qualified_name),
      optional(seq(kw("WITH"), kw("ENCRYPTION"))),
      optional(seq(kw("AUTHID"), choice(kw("DEFINER"), kw("CURRENT_USER")))),
      choice(kw("AS"), kw("IS")),
      repeat(choice(
        $.declaration,
        $.type_declaration,
        $.program_declaration,
        $.program_definition,
      )),
      optional(seq(kw("BEGIN"), repeat($.statement), optional($.exception_section))),
      kw("END"),
      optional(choice($.identifier, $.quoted_identifier)),
    )),
    trigger_definition: ($) => seq(
      kw("TRIGGER"),
      field("name", $.qualified_name),
      repeat($.ddl_fragment),
      $.block_body,
    ),
    program_declaration: ($) => prec.right(20, seq(
      choice(kw("PROCEDURE"), kw("FUNCTION")),
      // Share the definition prefix until ';' versus AS/IS resolves the branch.
      field("name", $.qualified_name),
      optional($.parameter_list),
      optional(seq(kw("RETURN"), $.type_name)),
      ";",
    )),
    parameter_list: ($) => prec.right(seq("(", optional(commaSep1($.parameter)), ")")),
    parameter: ($) => seq(
      field("name", $.identifier),
      optional(choice(kw("IN"), kw("OUT"), seq(kw("IN"), kw("OUT")))),
      field("type", $.type_name),
      optional(seq(choice(kw("DEFAULT"), ":=", "="), $.expression)),
    ),
    type_name: ($) => seq(
      $.qualified_name,
      optional(seq("(", commaSep1($.number), ")")),
      optional(choice(token.immediate("%TYPE"), token.immediate("%ROWTYPE"))),
    ),

    anonymous_block: ($) => seq(
      optional(seq(kw("DECLARE"), repeat($.declaration))),
      $.block_body,
    ),
    block_body: ($) => prec.right(seq(
      kw("BEGIN"),
      repeat($.statement),
      optional($.exception_section),
      kw("END"),
      optional(choice($.identifier, $.quoted_identifier)),
    )),
    declaration: ($) => choice(
      seq(field("name", $.identifier), optional(kw("CONSTANT")), field("type", $.type_name), optional(seq(choice(":=", kw("DEFAULT")), $.expression)), ";"),
      seq(kw("CURSOR"), field("name", $.identifier), optional($.parameter_list), kw("IS"), $.select_statement, ";"),
      seq(field("name", $.identifier), kw("EXCEPTION"), ";"),
    ),
    type_declaration: ($) => prec.right(seq(
      kw("TYPE"),
      field("name", $.identifier),
      kw("IS"),
      repeat1($.ddl_fragment),
      ";",
    )),
    exception_section: ($) => seq(
      kw("EXCEPTION"),
      repeat1(seq(kw("WHEN"), commaSep1($.qualified_name), kw("THEN"), repeat($.statement))),
    ),
    assignment_statement: ($) => seq(
      choice($.qualified_name, kw("NULL")),
      choice(":=", "="),
      $.expression,
    ),
    call_statement: ($) => $.call_expression,
    open_statement: ($) => prec.right(seq(
      kw("OPEN"),
      $.qualified_name,
      optional(seq("(", optional(commaSep1(choice($.expression, $.named_argument))), ")")),
      optional(seq(kw("FOR"), choice($.select_statement, $.expression))),
      optional(seq(kw("USING"), commaSep1($.expression))),
    )),
    fetch_statement: ($) => seq(
      kw("FETCH"),
      $.qualified_name,
      optional(choice(kw("NEXT"), kw("PRIOR"), kw("FIRST"), kw("LAST"))),
      kw("INTO"),
      commaSep1($.qualified_name),
    ),
    close_statement: ($) => seq(kw("CLOSE"), $.qualified_name),
    execute_immediate_statement: ($) => seq(
      kw("EXECUTE"),
      kw("IMMEDIATE"),
      $.expression,
      optional(seq(kw("INTO"), commaSep1($.qualified_name))),
      optional(seq(kw("USING"), commaSep1($.expression))),
      optional($.returning_clause),
    ),
    null_statement: () => kw("NULL"),
    raise_statement: ($) => prec.right(seq(kw("RAISE"), optional($.qualified_name))),
    return_statement: ($) => prec.right(seq(kw("RETURN"), optional($.expression))),
    if_statement: ($) => seq(
      kw("IF"), $.expression, kw("THEN"), repeat($.statement),
      repeat(seq(kw("ELSIF"), $.expression, kw("THEN"), repeat($.statement))),
      optional(seq(kw("ELSE"), repeat($.statement))),
      kw("END"), kw("IF"),
    ),
    loop_statement: ($) => seq(
      optional(choice(
        seq(kw("WHILE"), $.expression),
        seq(kw("FOR"), $.identifier, kw("IN"), $.expression),
      )),
      kw("LOOP"),
      repeat($.statement),
      kw("END"), kw("LOOP"),
    ),

    expression: ($) => choice(
      $.literal,
      $.bind_variable,
      $.qualified_name,
      $.call_expression,
      $.analytic_expression,
      $.parenthesized_expression,
      $.expression_list,
      $.subquery_expression,
      $.case_expression,
      $.between_expression,
      $.in_expression,
      $.is_expression,
      $.exists_expression,
      $.unary_expression,
      $.binary_expression,
    ),
    parenthesized_expression: ($) => seq("(", $.expression, ")"),
    expression_list: ($) => seq("(", $.expression, ",", commaSep1($.expression), ")"),
    subquery_expression: ($) => seq("(", $.select_statement, ")"),
    call_expression: ($) => prec(PREC.CALL, seq(
      field("function", $.qualified_name),
      "(",
      optional(commaSep1(choice($.expression, $.named_argument))),
      ")",
      optional($.within_group_clause),
    )),
    within_group_clause: ($) => seq(
      kw("WITHIN"), kw("GROUP"), "(", $.analytic_order_by_clause, ")",
    ),
    analytic_expression: ($) => prec(PREC.CALL, seq(
      $.call_expression,
      kw("OVER"),
      "(",
      optional($.partition_by_clause),
      optional($.analytic_order_by_clause),
      optional($.window_clause),
      ")",
    )),
    partition_by_clause: ($) => seq(kw("PARTITION"), kw("BY"), commaSep1($.expression)),
    analytic_order_by_clause: ($) => seq(
      kw("ORDER"),
      kw("BY"),
      commaSep1(seq(
        $.expression,
        optional(choice(kw("ASC"), kw("DESC"))),
        optional(seq(kw("NULLS"), choice(kw("FIRST"), kw("LAST")))),
      )),
    ),
    window_clause: ($) => seq(
      choice(kw("ROWS"), kw("RANGE")),
      choice(
        $.window_boundary,
        seq(kw("BETWEEN"), $.window_boundary, kw("AND"), $.window_boundary),
      ),
    ),
    window_boundary: ($) => choice(
      seq(kw("UNBOUNDED"), choice(kw("PRECEDING"), kw("FOLLOWING"))),
      seq(kw("CURRENT"), kw("ROW")),
      seq($.expression, choice(kw("PRECEDING"), kw("FOLLOWING"))),
    ),
    named_argument: ($) => seq($.identifier, "=>", $.expression),
    case_expression: ($) => seq(
      kw("CASE"),
      optional($.expression),
      repeat1(seq(kw("WHEN"), $.expression, kw("THEN"), $.expression)),
      optional(seq(kw("ELSE"), $.expression)),
      kw("END"),
    ),
    unary_expression: ($) => prec(PREC.UNARY, seq(choice("+", "-", kw("NOT"), kw("PRIOR")), $.expression)),
    between_expression: ($) => prec.right(PREC.COMPARE, seq(
      $.expression,
      optional(kw("NOT")),
      kw("BETWEEN"),
      $.expression,
      kw("AND"),
      $.expression,
    )),
    in_expression: ($) => prec.right(PREC.COMPARE, seq(
      $.expression,
      optional(kw("NOT")),
      kw("IN"),
      choice($.parenthesized_expression, $.expression_list, $.subquery_expression),
    )),
    is_expression: ($) => prec.right(PREC.COMPARE, seq(
      $.expression,
      kw("IS"),
      optional(kw("NOT")),
      choice(kw("NULL"), kw("TRUE"), kw("FALSE")),
    )),
    exists_expression: ($) => seq(kw("EXISTS"), $.subquery_expression),
    binary_expression: ($) => choice(
      prec.left(PREC.OR, seq($.expression, kw("OR"), $.expression)),
      prec.left(PREC.AND, seq($.expression, kw("AND"), $.expression)),
      prec.right(PREC.COMPARE, seq($.expression, choice("=", "<>", "!=", "<", "<=", ">", ">=", kw("LIKE"), kw("IN"), kw("IS")), $.expression)),
      prec.left(PREC.CONCAT, seq($.expression, "||", $.expression)),
      prec.left(PREC.ADD, seq($.expression, choice("+", "-"), $.expression)),
      prec.left(PREC.MULTIPLY, seq($.expression, choice("*", "/", "%"), $.expression)),
    ),

    qualified_name: ($) => choice(
      $.identifier,
      $.quoted_identifier,
      prec.right(PREC.MEMBER, seq(
        choice($.identifier, $.quoted_identifier),
        repeat1(seq(".", choice($.identifier, $.quoted_identifier))),
      )),
    ),
    member_expression: ($) => seq($.qualified_name, ".", $.identifier),
    bind_variable: () => token(seq(":", /[A-Za-z_][A-Za-z0-9_$#]*/)),
    literal: ($) => choice($.string, $.number, kw("NULL"), kw("TRUE"), kw("FALSE")),
    string: () => token(seq("'", repeat(choice(/[^']+/, "''")), "'")),
    number: () => token(/[0-9]+(\.[0-9]+)?([eE][+-]?[0-9]+)?/),
    quoted_identifier: () => token(seq('"', repeat(choice(/[^"]+/, '""')), '"')),
    // DM identifiers may contain _, $ and #. Give those complete names higher
    // lexical precedence so return_array_ is not split at the RETURN keyword.
    identifier: () => choice(
      token(prec(3, /_[A-Za-z0-9_$#]*/)),
      token(prec(3, /[A-Za-z][A-Za-z0-9]*[_$#][A-Za-z0-9_$#]*/)),
      token(prec(-1, /[A-Za-z][A-Za-z0-9]*/)),
    ),
    operator: () => token(choice(":=", "=>", "||", "<=", ">=", "<>", "!=", "+", "-", "*", "/", "%", "=")),
    line_comment: () => token(seq("--", /.*/)),
    block_comment: () => token(seq("/*", /[^*]*\*+([^/*][^*]*\*+)*/, "/")),

  },
});

function kw(word) {
  const pattern = new RegExp(word
    .split("")
    .map((letter) => `[${letter.toLowerCase()}${letter.toUpperCase()}]`)
    .join(""));
  return token(prec(2, pattern));
}

function commaSep1(rule) {
  return seq(rule, repeat(seq(",", rule)));
}

function fetchRows($) {
  return seq(
    kw("FETCH"),
    choice(kw("FIRST"), kw("NEXT")),
    $.expression,
    choice(kw("ROW"), kw("ROWS")),
    kw("ONLY"),
  );
}
