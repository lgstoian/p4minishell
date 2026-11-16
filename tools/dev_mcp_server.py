#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""dev_mcp_server.py - MCP dev plugin for P4MiniShell (host side).

A small Model Context Protocol server over stdio that exposes the repository's
own host tooling as MCP tools, so an editor agent (Cursor / Claude Code / Codex
/ OpenCode) can scaffold apps, build `.PKG` bundles, sign manifests, subset
fonts, pack images, and run the static guards without re-deriving commands.

Inspired by ESP-Brookesia's `@brookesia/app-dev-plugin`, but built on
P4MiniShell's existing tools (`newapp.py`, `make_pkg.py`, `pkg_sign.py`,
`font_subset.py`, `lvgl_image_pack.py`, `green_guard.py`, ...) — there is no
second implementation of any of them here.

Usage:
  python tools/dev_mcp_server.py            # stdio transport
  python tools/dev_mcp_server.py --list     # print the tool catalogue
"""

import argparse
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PY = sys.executable


def run(cmd, timeout=120):
    """Run a host tool and return (ok, text)."""
    try:
        proc = subprocess.run([PY] + cmd, cwd=ROOT, timeout=timeout,
                              capture_output=True, text=True)
    except subprocess.TimeoutExpired:
        return False, "timeout after %ss: %s" % (timeout, " ".join(cmd))
    except FileNotFoundError as exc:
        return False, "cannot run %s: %s" % (cmd[0], exc)
    out = (proc.stdout or "") + (("\n[stderr]\n" + proc.stderr) if proc.stderr else "")
    return proc.returncode == 0, out.strip()


TOOLS = [
    {
        "name": "p4_repo_status",
        "description": "Repository status: config version, board profiles, "
                       "whether an ESP-IDF install is reachable.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "p4_newapp",
        "description": "Scaffold a new native app component via tools/newapp.py.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "name": {"type": "string", "description": "app name"},
                "dir": {"type": "string", "description": "target directory"},
            },
            "required": ["name"],
        },
    },
    {
        "name": "p4_make_pkg",
        "description": "Build a single-file .PKG bundle archive (optionally signed).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "app": {"type": "string"},
                "src": {"type": "string"},
                "sign": {"type": "string", "description": "private key path"},
            },
            "required": ["app"],
        },
    },
    {
        "name": "p4_sign",
        "description": "Add an ECDSA P-256 SIGN line to a .ASSETS manifest.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "manifest": {"type": "string"},
                "key": {"type": "string"},
            },
            "required": ["manifest", "key"],
        },
    },
    {
        "name": "p4_verify_manifest",
        "description": "Verify a .ASSETS manifest signature against a public key.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "manifest": {"type": "string"},
                "pub": {"type": "string"},
            },
            "required": ["manifest", "pub"],
        },
    },
    {
        "name": "p4_font_subset",
        "description": "Build a CJK/English charset for font subsetting.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "output": {"type": "string"},
                "gb_level": {"type": "string", "enum": ["none", "l1", "l2", "gbk"]},
                "ui_strings": {"type": "array", "items": {"type": "string"}},
            },
            "required": ["output"],
        },
    },
    {
        "name": "p4_image_pack",
        "description": "Pack PNG/JPEG images into LVGL v9 .bin + index.json.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "input": {"type": "string"},
                "output": {"type": "string"},
                "c_header": {"type": "string"},
            },
            "required": ["input", "output"],
        },
    },
    {
        "name": "p4_docs_guards",
        "description": "Run the static guards (green_guard, comment_index, "
                       "breadcrumbs). No hardware needed.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "p4_firmware_build",
        "description": "Run `idf.py build` for the firmware (long-running).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "project": {"type": "string", "enum": ["firmware", "test"],
                            "description": "which IDF project to build"},
            },
        },
    },
]


def tool_repo_status(args):
    cfg = os.path.join(ROOT, "p4minishell_config.yaml")
    version = "(missing)"
    if os.path.exists(cfg):
        with open(cfg, encoding="utf-8") as f:
            for line in f:
                if line.startswith("config_version:"):
                    version = line.split(":", 1)[1].strip().strip('"')
                    break
    boards = sorted(d for d in os.listdir(os.path.join(ROOT, "boards"))
                    if os.path.isdir(os.path.join(ROOT, "boards", d)))
    idf = os.environ.get("IDF_PATH") or "(set IDF_PATH to build)"
    return True, "config_version: %s\nboards: %s\nIDF_PATH: %s\nroot: %s" % (
        version, ", ".join(boards), idf, ROOT)


def tool_newapp(args):
    return run(["tools/newapp.py", args["name"]])


def tool_make_pkg(args):
    cmd = ["tools/make_pkg.py", args["app"]]
    if args.get("src"):
        cmd += ["--src", args["src"]]
    if args.get("sign"):
        cmd += ["--sign", args["sign"]]
    return run(cmd)


def tool_sign(args):
    return run(["tools/pkg_sign.py", "sign", args["manifest"], "--key", args["key"]])


def tool_verify(args):
    return run(["tools/pkg_sign.py", "verify", args["manifest"], "--pub", args["pub"]])


def tool_font_subset(args):
    cmd = ["tools/font_subset.py", "--output", args["output"],
           "--gb-level", args.get("gb_level", "l1")]
    for s in args.get("ui_strings", []):
        cmd += ["--ui-strings", s]
    return run(cmd)


def tool_image_pack(args):
    cmd = ["tools/lvgl_image_pack.py", "--input", args["input"],
           "--output", args["output"]]
    if args.get("c_header"):
        cmd += ["--c-header", args["c_header"]]
    return run(cmd)


def tool_docs_guards(args):
    results = []
    all_ok = True
    for script in ("tools/green_guard.py", "tools/comment_index.py",
                   "tools/breadcrumbs.py"):
        ok, out = run([script], timeout=300)
        all_ok = all_ok and ok
        results.append("$ python %s -> %s\n%s" % (script, "ok" if ok else "FAIL",
                                                  out[-2000:]))
    return all_ok, "\n\n".join(results)


def tool_firmware_build(args):
    project = args.get("project", "firmware")
    cwd = ROOT if project == "firmware" else os.path.join(ROOT, "test")
    env = dict(os.environ)
    idf = env.get("IDF_PATH", r"C:\esp\v5.5.5\esp-idf")
    env["IDF_PATH"] = idf
    env.setdefault("IDF_COMPONENT_SUPPRESS_UNKNOWN_FILE_WARNINGS", "1")
    try:
        proc = subprocess.run(["idf.py", "build"], cwd=cwd, env=env,
                              timeout=1800, capture_output=True, text=True)
    except subprocess.TimeoutExpired:
        return False, "timeout after 1800s"
    except FileNotFoundError:
        return False, "idf.py not found (source %s/export.ps1 first)" % idf
    out = (proc.stdout or "") + (("\n[stderr]\n" + proc.stderr) if proc.stderr else "")
    return proc.returncode == 0, out[-4000:]


DISPATCH = {
    "p4_repo_status": tool_repo_status,
    "p4_newapp": tool_newapp,
    "p4_make_pkg": tool_make_pkg,
    "p4_sign": tool_sign,
    "p4_verify_manifest": tool_verify,
    "p4_font_subset": tool_font_subset,
    "p4_image_pack": tool_image_pack,
    "p4_docs_guards": tool_docs_guards,
    "p4_firmware_build": tool_firmware_build,
}


def handle(msg):
    method = msg.get("method")
    msg_id = msg.get("id")
    if method == "initialize":
        return {"jsonrpc": "2.0", "id": msg_id, "result": {
            "protocolVersion": "2024-11-05",
            "capabilities": {"tools": {}},
            "serverInfo": {"name": "p4minishell-dev", "version": "1.0"},
        }}
    if method in ("notifications/initialized", "notifications/cancelled"):
        return None
    if method == "ping":
        return {"jsonrpc": "2.0", "id": msg_id, "result": {}}
    if method == "tools/list":
        return {"jsonrpc": "2.0", "id": msg_id, "result": {"tools": TOOLS}}
    if method == "tools/call":
        params = msg.get("params") or {}
        name = params.get("name")
        fn = DISPATCH.get(name)
        if fn is None:
            return {"jsonrpc": "2.0", "id": msg_id, "result": {
                "content": [{"type": "text", "text": "unknown tool: %s" % name}],
                "isError": True}}
        try:
            ok, text = fn(params.get("arguments") or {})
        except Exception as exc:  # noqa: BLE001 - report, never crash the server
            ok, text = False, "tool error: %r" % (exc,)
        return {"jsonrpc": "2.0", "id": msg_id, "result": {
            "content": [{"type": "text", "text": text}],
            "isError": not ok}}
    if msg_id is not None:
        return {"jsonrpc": "2.0", "id": msg_id,
                "error": {"code": -32601, "message": "method not found: %s" % method}}
    return None


def main():
    ap = argparse.ArgumentParser(description="P4MiniShell MCP dev plugin.")
    ap.add_argument("--list", action="store_true", help="print the tool catalogue")
    args = ap.parse_args()
    if args.list:
        for t in TOOLS:
            print("%-22s %s" % (t["name"], t["description"]))
        return 0
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            continue
        resp = handle(msg)
        if resp is not None:
            sys.stdout.write(json.dumps(resp) + "\n")
            sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
