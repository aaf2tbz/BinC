# tools/tree-sitter-binc — BinC grammar for tree-sitter

Highlights (and, later, folding/indentation) for the BinC language, generated
against the EBNF in `SPEC.md`.

Build and test:

    cd tools/tree-sitter-binc
    npm install               # tree-sitter-cli
    npx tree-sitter generate
    npx tree-sitter build     # produces build/grammar.node
    npm test                  # parses every examples/*.binc, flags ERROR nodes

The test script skips gracefully (exit 0) when the bindings are not built, so
it is safe to run anywhere.
