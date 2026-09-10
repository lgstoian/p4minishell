#!/usr/bin/env python3
"""bounce_twice.py - run BOUNCE twice; catch a re-init crash.

Usage: python bounce_twice.py [COMx]
"""
import sys
import time

sys.path.insert(0, "tools")
from bg_run import boot, run_quiet


def run_once(sh, tag):
    sh.s.reset_input_buffer()
    sh.s.write(b"BOUNCE\r\n")
    sh.s.timeout = 0.5
    end = time.time() + 90
    data = b""
    while time.time() < end:
        chunk = sh.s.read(65536)
        if chunk:
            data += chunk
            for p in (b"Guru Meditation", b"Stack protection", b"Backtrace",
                      b"assert failed", b"abort()", b"Rebooting"):
                if p in data:
                    print("%s PANIC: %r" % (tag, p), flush=True)
                    print(data[-1500:].decode("utf-8", errors="replace"), flush=True)
                    return False
            if b"M-BOUNCE-DONE" in data:
                text = data.decode("utf-8", errors="replace")
                print("%s done frames=%d" % (tag, text.count("M-BOUNCE-FRAME")), flush=True)
                return True
    print("%s TIMEOUT, tail:" % tag, flush=True)
    print(data[-800:].decode("utf-8", errors="replace"), flush=True)
    return False


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    sh = boot(port)
    run_quiet(sh, "dir BOUNCE.BAT")
    ok1 = run_once(sh, "pass1")
    ok2 = run_once(sh, "pass2") if ok1 else False
    print("RESULT pass1=%s pass2=%s" % (ok1, ok2), flush=True)
    sh.close()


main()
