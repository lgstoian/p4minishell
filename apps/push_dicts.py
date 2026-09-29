#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""push_dicts.py - push spellcheck wordlists to sd:/DICTS/.

Wordlists back the `edit` spellcheck underlines (writerdeck). Each
apps/dicts/<NAME>.words lands at sd:/DICTS/<NAME>.words (one lower-case
word per line; the default list is `en`).

The user overlay (sd:/DICTS/user.words, words learned on-device via
`spell learn` / the editor AddWord key) travels the other way:
`pull` downloads it with tools/pull.py and `merge` unions it into the
local file so learned words survive re-pushes.

Usage:
  python push_dicts.py [COMx]
  python push_dicts.py pull [COMx] [name]    (default: user.words)
  python push_dicts.py merge [COMx] [name]   (default: user.words)
"""
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "companion"))
from push_sd import push_file, wait_shell, open_port  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DICTS = os.path.join(HERE, "dicts")


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM11"
    names = [n for n in sorted(os.listdir(DICTS)) if n.endswith(".words")]
    if not names:
        print("FAIL: no wordlists in %s" % DICTS)
        return 1

    ser = open_port(port, 115200, 1)
    ser.reset_input_buffer()
    if not wait_shell(ser):
        print("FAIL: shell not responding on %s" % port)
        ser.close()
        return 1
    # The open above usually reboots the board (DTR transition); let it settle.
    time.sleep(20.0)
    ser.reset_input_buffer()
    ser.write(b"md DICTS\n")
    time.sleep(0.5)
    ser.read(ser.in_waiting or 1)

    ok = True
    for name in names:
        with open(os.path.join(DICTS, name), "rb") as f:
            data = f.read()
        target = "DICTS/" + name
        pushed = False
        for attempt in range(3):
            if push_file(ser, target, data):
                print("PUSH ok   %s (%d bytes)" % (target, len(data)))
                pushed = True
                break
            print("  retry %s (%d/3)" % (target, attempt + 1))
            time.sleep(2.0)
        if not pushed:
            print("PUSH FAIL %s" % target)
            ok = False
    ser.close()
    print("RESULT %s" % ("OK" if ok else "FAIL"))
    return 0 if ok else 1


def cmd_pull(port, name):
    """Download sd:/DICTS/<name> to apps/dicts/<name> via tools/pull.py."""
    local = os.path.join(DICTS, name)
    pull = os.path.join(ROOT, "tools", "pull.py")
    r = subprocess.run([sys.executable, pull, "DICTS/" + name, local, port])
    if r.returncode != 0 or not os.path.isfile(local):
        print("FAIL: pull DICTS/%s" % name)
        return 1
    print("PULL ok   DICTS/%s (%d bytes)" % (name, os.path.getsize(local)))
    return 0


def cmd_merge(port, name):
    """Union the on-device user overlay into the local file (order kept,
    local first; duplicates dropped, comparison case-insensitive)."""
    local = os.path.join(DICTS, name)
    tmp = tempfile.NamedTemporaryFile(delete=False, suffix=".words")
    tmp.close()
    pull = os.path.join(ROOT, "tools", "pull.py")
    r = subprocess.run([sys.executable, pull, "DICTS/" + name, tmp.name, port])
    if r.returncode != 0:
        print("FAIL: pull DICTS/%s for merge" % name)
        os.unlink(tmp.name)
        return 1
    seen = set()
    merged = []
    for path in (local, tmp.name):
        if not os.path.isfile(path):
            continue
        with open(path, "rb") as f:
            for raw in f.read().decode("utf-8", "replace").splitlines():
                word = raw.strip().lower()
                if word and word not in seen:
                    seen.add(word)
                    merged.append(word)
    os.unlink(tmp.name)
    added = 0
    with open(local, "w", encoding="utf-8", newline="\n") as f:
        for word in merged:
            f.write(word + "\n")
            added += 1
    print("MERGE ok  %s (%d words)" % (local, added))
    return 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] in ("pull", "merge"):
        mode = sys.argv[1]
        rest = sys.argv[2:]
        port = rest[0] if rest else "COM11"
        name = rest[1] if len(rest) > 1 else "user.words"
        if mode == "pull":
            sys.exit(cmd_pull(port, name))
        sys.exit(cmd_merge(port, name))
    sys.exit(main())
