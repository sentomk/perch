#!/usr/bin/env python3
"""Drive textDocument/definition against kinglet-lsp.

Each case directory contains:
    request.json - {
        "target_file": str,    // relative path inside case-dir of the .kl
                               // file to didOpen (e.g. "app.kl")
        "line": int,
        "character": int
    }
    expected.json - {"file": str, "line": int}
                    `file` is the case-dir-relative path the returned
                    Location's URI MUST resolve to; `line` is the 1-based
                    source line the Location's range MUST start on.

This exists to catch cross-file "go to definition" regressions: symbols
resolved through `import <module>;` must return a Location pointing at the
*defining* file (and the `export`/`pub` declaration line), not the
importing file's `import` statement.
"""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path
from urllib.parse import unquote, urlparse


def read_message(stream) -> dict:
    headers: dict[str, str] = {}
    while True:
        line = stream.readline()
        if not line:
            raise EOFError("LSP server closed stdout")
        line = line.decode("utf-8").strip()
        if not line:
            break
        name, value = line.split(":", 1)
        headers[name.strip().lower()] = value.strip()
    length = int(headers["content-length"])
    body = stream.read(length)
    if not body:
        raise EOFError("empty LSP message body")
    return json.loads(body.decode("utf-8"))


def write_message(stream, payload: dict) -> None:
    body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    header = f"Content-Length: {len(body)}\r\n\r\n".encode("utf-8")
    stream.write(header + body)
    stream.flush()


def request(proc, req_id: int, method: str, params) -> dict:
    write_message(
        proc.stdin,
        {"jsonrpc": "2.0", "id": req_id, "method": method, "params": params},
    )
    while True:
        message = read_message(proc.stdout)
        if message.get("id") == req_id:
            if "error" in message:
                raise RuntimeError(f"{method} failed: {message['error']}")
            return message["result"]


def notify(proc, method: str, params: dict) -> None:
    write_message(proc.stdin, {"jsonrpc": "2.0", "method": method, "params": params})


def uri_to_path(uri: str) -> Path:
    parsed = urlparse(uri)
    return Path(unquote(parsed.path))


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <kinglet-lsp> <case-dir>", file=sys.stderr)
        return 2

    lsp_path = Path(sys.argv[1])
    case_dir = Path(sys.argv[2])
    request_spec = json.loads((case_dir / "request.json").read_text(encoding="utf-8"))
    expected = json.loads((case_dir / "expected.json").read_text(encoding="utf-8"))

    target_file = request_spec.get("target_file", "initial.kl")
    initial_path = (case_dir / target_file).resolve()
    if not initial_path.exists():
        print(f"FAIL: target file missing: {initial_path}", file=sys.stderr)
        return 2

    initial = initial_path.read_text(encoding="utf-8")
    uri = initial_path.as_uri()

    proc = subprocess.Popen(
        [str(lsp_path)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert proc.stdin and proc.stdout

    try:
        request(proc, 1, "initialize", {"processId": None, "rootUri": None, "capabilities": {}})
        notify(proc, "initialized", {})
        notify(
            proc,
            "textDocument/didOpen",
            {
                "textDocument": {
                    "uri": uri,
                    "languageId": "kinglet",
                    "version": 1,
                    "text": initial,
                }
            },
        )
        result = request(
            proc,
            2,
            "textDocument/definition",
            {
                "textDocument": {"uri": uri},
                "position": {
                    "line": request_spec["line"],
                    "character": request_spec["character"],
                },
            },
        )
    finally:
        try:
            request(proc, 99, "shutdown", None)
        except Exception:
            pass
        notify(proc, "exit", {})
        proc.terminate()
        proc.wait(timeout=5)

    if result is None:
        print("FAIL: textDocument/definition returned null (no definition found)", file=sys.stderr)
        return 1

    # A single Location or a Location[]; normalize to the first entry.
    location = result[0] if isinstance(result, list) else result
    got_path = uri_to_path(location["uri"])
    expected_path = (case_dir / expected["file"]).resolve()
    got_line = location["range"]["start"]["line"] + 1  # LSP is 0-based

    failed = False
    if got_path != expected_path:
        print(
            f"FAIL: expected definition in {expected_path} but got {got_path}",
            file=sys.stderr,
        )
        failed = True
    if got_line != expected["line"]:
        print(
            f"FAIL: expected definition at line {expected['line']} but got line {got_line}",
            file=sys.stderr,
        )
        failed = True

    return 1 if failed else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
