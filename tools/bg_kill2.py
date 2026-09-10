#!/usr/bin/env python3
"""bg_kill2.py - start bg delay, verify alive, kill it, verify alive.

Usage: python bg_kill2.py [COMx]
"""
import sys
import time

sys.path.insert(0, "tools")
from shell_session import Shell


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    sh = Shell(port)
    time.sleep(12)
    sh.s.reset_input_buffer()
    for cmd in ["echo A1", "start delay 20000", "echo A2", "taskkill bg0",
                "echo A3", "taskkill bg0", "echo A4"]:
        try:
            out = sh.run(cmd, timeout=25)
        except RuntimeError as e:
            print("PANIC after %r: %s" % (cmd, e), flush=True)
            break
        print("=== %s ===" % cmd, flush=True)
        print(out[-500:], flush=True)
    sh.close()


main()
