#!/usr/bin/env python3
# Real end-to-end test of nova_lsp: spawns the actual compiled binary,
# writes real Content-Length-framed JSON-RPC messages to its stdin, and
# reads its real stdout responses back — no mocking of the LSP server
# itself.
import subprocess
import sys

def make_message(json_body: str) -> bytes:
    body = json_body.encode("utf-8")
    header = f"Content-Length: {len(body)}\r\n\r\n".encode("utf-8")
    return header + body

def read_message(stream) -> str:
    headers = {}
    while True:
        line = stream.readline().decode("utf-8")
        if line in ("\r\n", "\n", ""):
            break
        if ":" in line:
            k, v = line.split(":", 1)
            headers[k.strip()] = v.strip()
    length = int(headers.get("Content-Length", "0"))
    body = stream.read(length).decode("utf-8")
    return body

proc = subprocess.Popen(
    ["./nova-lsp"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    bufsize=0,
)

def send(json_body: str):
    proc.stdin.write(make_message(json_body))
    proc.stdin.flush()

def recv() -> str:
    return read_message(proc.stdout)

# ---- 1. initialize ----
send('{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}}')
resp1 = recv()
print("=== initialize response ===")
print(resp1)
assert '"hoverProvider":true' in resp1, "FAIL: hoverProvider capability missing"
assert '"textDocumentSync":1' in resp1, "FAIL: textDocumentSync capability missing"
print("PASS: initialize response has expected capabilities\n")

# ---- 2. initialized notification (no response expected) ----
send('{"jsonrpc":"2.0","method":"initialized","params":{}}')

# ---- 3. didOpen with a file containing real lint-triggering issues ----
# Build the didOpen JSON via json.dumps so the source text (which itself
# contains real newlines and a quote) is escaped correctly for embedding —
# far less error-prone than hand-escaping nested quotes here.
import json as _json
source_text = 'import Nova.web\nname = "Javed"   \nNova.show(name)\n'
didopen_params = {
    "textDocument": {
        "uri": "file:///test.nova",
        "languageId": "nova",
        "version": 1,
        "text": source_text,
    }
}
didopen = _json.dumps({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": didopen_params})
send(didopen)
diag_notification = recv()
print("=== publishDiagnostics after didOpen ===")
print(diag_notification)
assert '"method":"textDocument/publishDiagnostics"' in diag_notification, "FAIL: expected publishDiagnostics notification"
assert "unused import" in diag_notification, "FAIL: expected unused-import diagnostic for Nova.web"
assert "trailing whitespace" in diag_notification, "FAIL: expected trailing-whitespace diagnostic"
print("PASS: diagnostics correctly flagged unused import + trailing whitespace\n")

# ---- 4. hover over "name" (line 1, character 1 in the 0-based doc: "name = ...") ----
hover_req = '{"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///test.nova"},"position":{"line":1,"character":1}}}'
send(hover_req)
hover_resp = recv()
print("=== hover response ===")
print(hover_resp)
assert "Nova Native Element" in hover_resp, "FAIL: expected placeholder hover text"
assert "name" in hover_resp, "FAIL: expected the actual hovered word 'name' in the tooltip"
print("PASS: hover returned real word-under-cursor enrichment\n")

# ---- 5. shutdown + exit ----
send('{"jsonrpc":"2.0","id":3,"method":"shutdown","params":null}')
shutdown_resp = recv()
assert '"result":null' in shutdown_resp, "FAIL: shutdown should respond with null result"
print("PASS: shutdown responded correctly\n")

send('{"jsonrpc":"2.0","method":"exit","params":null}')
proc.stdin.close()
exit_code = proc.wait(timeout=5)
print(f"Server exited with code {exit_code}")
assert exit_code == 0, "FAIL: server should exit cleanly after exit notification"

print("\nALL LSP TESTS PASSED")
