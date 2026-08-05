/* tools/tree-sitter-binc/grammar.js — tree-sitter grammar for BinC.
 * Generated against SPEC.md; highlights the C-like surface of the language. */
module.exports = grammar({
  name: "binc",

  extras: $ => [
    /\s/,
    $.comment,
  ],

  conflicts: $ => [
    [$.type, $.expression],
    [$.type, $.primary],       /* `(float)x` cast vs parenthesized expression */
    [$.scalar_type, $.type],   /* `constant` begins both the const-def and the addr-space form */
    [$._statement, $.statement_or_block],  /* dangling-else style ambiguity */
    [$.if_statement, $.statement_or_block],
  ],

  rules: {
    source_file: $ => repeat($._top_level),

    _top_level: $ => choice(
      $.function_definition,
      $.struct_definition,
      $.constant_definition,
      $.include_statement,
      $.once_statement,
    ),

    include_statement: $ => seq("include", $.string, ";"),
    once_statement: $ => seq("once", ";"),
    string: $ => /"(\\.|[^"\\])*"/,

    template_head: $ => seq(
      "template", "<", "typename", $.identifier, ">",
    ),

    struct_definition: $ => seq(
      optional($.template_head),
      "struct", $.identifier, "{", repeat($.field_declaration), "}", ";",
    ),

    field_declaration: $ => seq(
      $.type, $.identifier,
      repeat($.array_declarator),
      optional($.attribute),
      repeat(seq(",", $.identifier, repeat($.array_declarator), optional($.attribute))),
      ";",
    ),

    attribute: $ => seq(
      "[[", field("name", $.identifier),
      optional(seq("(", choice($.number, $.identifier), ")")),
      "]]",
    ),

    constant_definition: $ => seq(
      "constant", $.scalar_type, $.identifier, "=", choice($.number, "true", "false"), ";",
    ),

    scalar_type: $ => choice("void", "bool", "float", "half", "int", "uint"),

    /* keyword+digit spellings (float4, mat2, coord1D) — a single token so the
     * lexer's keyword preference doesn't split them */
    numeric_type: $ => token(choice(
      /float[2-4]/, /int[2-4]/, /uint[2-4]/, /mat[2-4]/, /coord[123]D/,
    )),

    function_definition: $ => seq(
      optional($.template_head),
      optional(choice("vertex", "fragment", "kernel")),
      $.type, $.identifier,
      "(", optional(seq($.parameter, repeat(seq(",", $.parameter)))), ")",
      $.block,
    ),

    parameter: $ => seq(
      optional(choice("uniform", "varying")),
      $.type, $.identifier,
      repeat($.array_declarator),
    ),

    array_declarator: $ => seq("[", $.number, "]"),

    block: $ => seq("{", repeat($._statement), "}"),

    _statement: $ => choice(
      $.block,
      $.declaration,
      $.expression_statement,
      $.return_statement,
      $.if_statement,
      $.while_statement,
      $.do_statement,
      $.for_statement,
      $.switch_statement,
      $.break_statement,
      $.continue_statement,
    ),

    declaration: $ => seq($._declaration, ";"),
    _declaration: $ => seq(
      optional("const"),
      $.type, $.identifier, repeat($.array_declarator),
      optional(seq("=", $.expression)),
    ),

    expression_statement: $ => seq($.expression, ";"),

    return_statement: $ => seq("return", optional($.expression), ";"),
    break_statement: $ => seq("break", ";"),
    continue_statement: $ => seq("continue", ";"),

    if_statement: $ => prec.left(5, seq(
      "if", "(", $.expression, ")", $.statement_or_block,
      optional(seq("else", $.statement_or_block)),
    )),

    while_statement: $ => seq("while", "(", $.expression, ")", $.statement_or_block),
    do_statement: $ => seq("do", $.statement_or_block, "while", "(", $.expression, ")", ";"),

    for_statement: $ => seq(
      "for", "(",
      optional(choice($._declaration, $.expression)), ";",
      optional($.expression), ";",
      optional($.expression),
      ")", $.statement_or_block,
    ),

    switch_statement: $ => seq(
      "switch", "(", $.expression, ")", "{",
      repeat(choice(
        seq("case", $.expression, ":", repeat($._statement)),
        seq("default", ":", repeat($._statement)),
      )),
      "}",
    ),

    statement_or_block: $ => choice($.block, $._statement),

    type: $ => seq(
      optional(choice("device", "constant", "threadgroup", "thread")),
      choice(
        "void", "bool", "float", "half", "int", "uint",
        "mat", "coord", "grid_extent", "atomic", "texture2d", "sampler",
        $.numeric_type,
        $.identifier,
      ),
      optional(seq("<", $.type, ">")),
      repeat("*"),
    ),

    expression: $ => choice(
      $.assignment,
      $.ternary,
    ),

    assignment: $ => prec.right(1, seq(
      $.unary,
      choice("=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="),
      $.expression,
    )),

    ternary: $ => prec.left(1, seq(
      $.binary,
      optional(seq("?", $.expression, ":", $.ternary)),
    )),

    binary: $ => prec.left(2, seq(
      $.unary,
      repeat(seq(choice(
        "+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>",
        "<", "<=", ">", ">=", "==", "!=", "&&", "||",
      ), $.unary)),
    )),

    unary: $ => prec(3, choice(
      seq(choice("!", "-", "~", "*"), $.unary),
      seq("(", $.type, ")", $.unary),
      $.postfix,
    )),

    postfix: $ => prec(4, seq(
      $.primary,
      repeat(choice(
        seq("(", optional(seq($.expression, repeat(seq(",", $.expression)))), ")"),
        seq("[", $.expression, "]"),
        seq(".", $.identifier),
        seq("->", $.identifier),
        seq(choice("++", "--")),
      )),
    )),

    primary: $ => choice(
      $.number,
      "true", "false",
      $.numeric_type,   /* vector/matrix constructors: float4(...) */
      $.identifier,
      seq("(", $.expression, ")"),
    ),

    identifier: $ => /[A-Za-z_][A-Za-z0-9_]*/,
    number: $ => choice(
      /0[xX][0-9a-fA-F]+[uU]?/,
      /[0-9]+\.[0-9]*([eE][+-]?[0-9]+)?[fF]?/,
      /\.[0-9]+([eE][+-]?[0-9]+)?[fF]?/,
      /[0-9]+([eE][+-]?[0-9]+)?[fF]?/,
      /[0-9]+[uU]/,
    ),

    comment: $ => choice(
      seq("//", /[^\n]*/),
      seq("/*", /[^*]*\*+([^/*][^*]*\*+)*/, "/"),
    ),
  },
});
