// tools/binc-lsp/README.md — minimal LSP server for BinC
//
// binc_lsp.py speaks LSP 3.17 over stdio. It drives the real compiler in
// -fsyntax-only mode and publishes the located diagnostics (line/col/message)
// that the Phase 0 error infrastructure produces.
//
// Try it with any LSP client that can launch a custom server, e.g. Neovim:
//
//   :LspStart  (after configuring a binc filetype + the command
//               `python3 <repo>/tools/binc-lsp/binc_lsp.py`)
//
// or smoke-test it from a terminal with a framed initialize request:
//
//   printf 'Content-Length: 83\r\n\r\n{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
//     | python3 binc_lsp.py
//
// Limitations: diagnostics on open/change only; no hover, completion, or
// go-to-definition. That is the scaffold's point — the compiler's error list
// is the single source of truth and every future feature plugs into it.
