#!/usr/bin/env python3
"""match_debug.py - dump raw/clean bytes around a prompt."""
import re
import sys
import time

sys.path.insert(0, "tools")
from shell_session import Shell, PROMPT_RE

ANSI = re.compile(rb"\x1b\[[0-9;]*m")


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    sh = Shell(port)
    time.sleep(12)
    sh.s.reset_input_buffer()
    sh.s.write(b"echo HI\r\n")
    time.sleep(0.6)
    out = b""
    end = time.time() + 6
    while time.time() < end:
        chunk = sh.s.read(4096)
        if chunk:
            out += chunk
    clean = ANSI.sub(b"", out)
    print("RAW tail:", repr(out[-160:]), flush=True)
    print("CLEAN tail:", repr(clean[-160:]), flush=True)
    i = clean.find(b"echo HI")
    print("echo_at:", i, flush=True)
    m = PROMPT_RE.search(clean, i if i >= 0 else 0)
    print("prompt match:", repr(m.group(0)) if m else None, flush=True)
    sh.close()


main()
