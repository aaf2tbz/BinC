#!/usr/bin/env node
/* tools/tree-sitter-binc/test/parse_all.js — parse every example with
 * tree-sitter-binc and report trees containing ERROR nodes. Requires the
 * grammar to be built (tree-sitter generate + node bindings). Skips with a
 * notice if the bindings are unavailable. */
const fs = require("fs");
const path = require("path");

const ROOT = path.resolve(__dirname, "..", "..", "..");
const EXAMPLES = path.join(ROOT, "examples");

function loadParser() {
  try {
    // Built via: npx tree-sitter generate && npx tree-sitter build
    return require(path.join(__dirname, "..", "build", "grammar.node"));
  } catch (e) {
    try {
      return require("tree-sitter-binc");
    } catch (e2) {
      return null;
    }
  }
}

const parser = loadParser();
if (!parser) {
  console.log(
    "SKIP: tree-sitter-binc bindings not built. Run:\n" +
    "  cd tools/tree-sitter-binc && npx tree-sitter generate && npx tree-sitter build"
  );
  process.exit(0);
}

const files = fs.readdirSync(EXAMPLES).filter((f) => f.endsWith(".binc"));
let failed = 0;
for (const f of files) {
  const src = fs.readFileSync(path.join(EXAMPLES, f), "utf8");
  const tree = parser.parse(src);
  const errors = [];
  function walk(node) {
    if (node.type === "ERROR" || node.isMissing) errors.push(node);
    for (const child of node.children) walk(child);
  }
  walk(tree.rootNode);
  if (errors.length) {
    failed++;
    console.log(`FAIL: ${f} — ${errors.length} ERROR/missing node(s)`);
    for (const e of errors.slice(0, 3)) {
      console.log(`   at ${e.startPosition.row + 1}:${e.startPosition.column + 1} (${e.type})`);
    }
  } else {
    console.log(`PASS: ${f}`);
  }
}
console.log(failed ? `${failed} example(s) failed to parse` : `all ${files.length} examples parse cleanly`);
process.exit(failed ? 1 : 0);
