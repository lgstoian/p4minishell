#!/usr/bin/env python3
"""bg_crash_repro.py - start a bg delay, then only listen for death.

Usage: python bg_crash_repro.py [COMx]
"""
import sys
import time

sys.path.insert(0, "tools")
from shell_session import Shell, open_port


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    sh = Shell(port)
    time.sleep(12)
    sh.s.reset_input_buffer()
    print("--- start ---", flush=True)
    print(sh.run("start delay 8000", timeout=15)[-300:], flush=True)
    # Stop sending; just listen for 20 s (panic/backtrace/reboot?).
    ser = sh.s
    ser.timeout = 0.5
    end = time.time() + 20
    data = b""
    while time.time() < end:
        chunk = ser.read(4096)
        if chunk:
            data += chunk
    print("--- 20 s of silence: %d bytes ---" % len(data), flush=True)
    print(data.decode("utf-8", errors="replace")[-3000:], flush=True)
    ser.close()


main()
