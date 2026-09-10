#!/usr/bin/env python3
"""elite_test.py - drive ELITE: save + quit + save-file assert.

Usage: python elite_test.py [COMx]
Raw-driven (modals eat run_quiet markers): launch, pick Save (4),
then Quit (5), then type ELITE.SAV and check the credits line.
Serial list numbers are 1-based; errorlevels are 0-based.
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


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    sh = boot(port)
    sh.s.timeout = 0.5
    sh.s.reset_input_buffer()
    sh.s.write(b"ELITE\r\n")
    data = wait_for(sh, "M-ELITE-DOCKED")
    print("docked=%s" % (b"M-ELITE-DOCKED" in data), flush=True)
    time.sleep(2)
    sh.s.write(b"4\r\n")  # Save
    data = wait_for(sh, "M-ELITE-SAVED")
    print("saved=%s" % (b"M-ELITE-SAVED" in data), flush=True)
    time.sleep(2)
    sh.s.write(b"5\r\n")  # Quit
    data = wait_for(sh, "M-ELITE-DONE")
    done = b"M-ELITE-DONE" in data
    print("done=%s" % done, flush=True)
    text = data.decode("utf-8", errors="replace")
    i = text.find("M-ELITE-DONE")
    if i >= 0:
        print(text[max(0, i - 60):i + 50][-120:], flush=True)
    if done:
        sh.s.reset_input_buffer()
        sh.s.write(b"type ELITE.SAV\r\n")
        time.sleep(3)
        out = sh.s.read(65536).decode("utf-8", errors="replace")
        print("SAV has credits=%s" % ("set credits=" in out), flush=True)
        print(out[-300:], flush=True)
    sh.close()


main()
