#!/usr/bin/env python3
"""binc_lsp.py — minimal Language Server Protocol scaffold for BinC.

Speaks LSP 3.17 over stdio. On textDocument/didOpen and textDocument/didChange
it writes the buffer to a temp file, runs `binc -fsyntax-only`, parses the
located diagnostics from stderr, and publishes them. No code actions, no
hover — just live diagnostics from the real compiler.

Usage:  binc-lsp [path-to-binc]     (default: ./binc next to this script)
"""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
BINC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "..", "binc", "binc")
BINC = os.path.abspath(BINC)

ERR_RE = None  # replaced by the regex below once 're' is imported
import re
ERR_RE = re.compile(r"^binc: error \(line (\d+)(?:, col (\d+))?\): (.*)$")


def diagnostics_for(text: str):
    """Run the compiler in -fsyntax-only mode and return LSP diagnostics."""
    with tempfile.NamedTemporaryFile("w", suffix=".binc", delete=False) as fh:
        fh.write(text)
        path = fh.name
    try:
        proc = subprocess.run([BINC, "-fsyntax-only", path],
                              capture_output=True, text=True, timeout=15)
        out = proc.stderr
    except (OSError, subprocess.TimeoutExpired) as exc:
        out = f"binc: error: {exc}"
    finally:
        os.unlink(path)
    diags = []
    for line in out.splitlines():
        m = ERR_RE.match(line.strip())
        if m:
            line_no = int(m.group(1)) - 1
            col = int(m.group(2)) - 1 if m.group(2) else 0
            diags.append({
                "range": {
                    "start": {"line": line_no, "character": col},
                    "end": {"line": line_no, "character": col + 1},
                },
                "severity": 1,
                "source": "binc",
                "message": m.group(3),
            })
    return diags


def send(msg):
    payload = json.dumps(msg).encode("utf-8")
    sys.stdout.buffer.write(b"Content-Length: %d\r\n\r\n" % len(payload))
    sys.stdout.buffer.write(payload)
    sys.stdout.buffer.flush()


def read_message():
    headers = {}
    line = sys.stdin.buffer.readline()
    if not line:
        return None
    while line not in (b"\r\n", b"\n", b""):
        k, _, v = line.decode("utf-8").partition(":")
        headers[k.strip().lower()] = v.strip()
        line = sys.stdin.buffer.readline()
    length = int(headers.get("content-length", 0))
    body = sys.stdin.buffer.read(length) if length else b"{}"
    return json.loads(body)


def main():
    docs = {}  # uri -> text
    while True:
        msg = read_message()
        if msg is None:
            return 0
        method = msg.get("method")
        params = msg.get("params", {})
        msg_id = msg.get("id")

        if method == "initialize":
            send({"jsonrpc": "2.0", "id": msg_id, "result": {
                "capabilities": {
                    "textDocumentSync": {"openClose": True, "change": 1},
                    "diagnosticProvider": False,
                },
                "serverInfo": {"name": "binc-lsp", "version": "0.1.0"},
            }})
        elif method == "initialized":
            pass
        elif method == "textDocument/didOpen":
            uri = params["textDocument"]["uri"]
            docs[uri] = params["textDocument"]["text"]
            send({"jsonrpc": "2.0", "method": "textDocument/publishDiagnostics",
                  "params": {"uri": uri, "diagnostics": diagnostics_for(docs[uri])}})
        elif method == "textDocument/didChange":
            uri = params["textDocument"]["uri"]
            for change in params.get("contentChanges", []):
                if "text" in change:
                    docs[uri] = change["text"]
            send({"jsonrpc": "2.0", "method": "textDocument/publishDiagnostics",
                  "params": {"uri": uri, "diagnostics": diagnostics_for(docs[uri])}})
        elif method == "textDocument/didClose":
            docs.pop(params["textDocument"]["uri"], None)
        elif method == "shutdown":
            send({"jsonrpc": "2.0", "id": msg_id, "result": None})
        elif method == "exit":
            return 0


if __name__ == "__main__":
    sys.exit(main())
