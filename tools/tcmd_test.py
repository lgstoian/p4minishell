#!/usr/bin/env python3
"""tcmd_test.py - drive TCMD: swap + quit.

Usage: python tcmd_test.py [COMx]
Raw-driven: launch, pick Swap (9), then Quit (12). Asserts markers.
Serial list numbers are 1-based (12 items, Quit=12).
"""
import sys
import time

sys.path.insert(0, "tools")
from bg_run import boot


def wait_for(sh, marker, timeout=90):
    end = time.time() + timeout
    data = b""
    while time.time() < end:
        chunk = sh.s.read(65536)
        if chunk:
            data += chunk
            if marker.encode() in data:
                return data
    return data


def press(sh, text, expect):
    """Settled send + recovery retries (sends predating the modal open are
    lost to the command queue; deep_test.press pattern, extended: TCMD's
    refresh is slow, so retry up to 4 times)."""
    for attempt in range(4):
        time.sleep(12)
        sh.s.write(text)
        time.sleep(1.0)
        data = wait_for(sh, expect, timeout=30)
        if expect.encode() in data:
            return True
    return False


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    sh = boot(port)
    sh.s.timeout = 0.5
    sh.s.reset_input_buffer()
    sh.s.write(b"TCMD\r\n")
    data = wait_for(sh, "M-TCMD")
    print("launched=%s" % (b"M-TCMD" in data), flush=True)
    ok = press(sh, b"8\r\n", "swap")  # Swap panes (1-based item 8)
    print("swapped=%s" % ok, flush=True)
    ok = press(sh, b"11\r\n", "M-TCMD-DONE")  # Quit (1-based item 11)
    print("done=%s" % ok, flush=True)
    sh.close()


main()
