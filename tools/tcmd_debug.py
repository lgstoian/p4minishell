#!/usr/bin/env python3
"""tcmd_debug.py - raw capture of a TCMD run to find the crash point."""
import sys
import time

sys.path.insert(0, "tools")
from shell_session import open_port

PANIC = [b"Guru Meditation", b"Backtrace", b"abort()", b"assert failed",
         b"Stack protection", b"Task watchdog", b"StoreProhibited"]


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    ser = open_port(port)
    time.sleep(12)
    while True:
        if not ser.read(65536):
            break
    ser.write(b"TCMD\r\n")
    buf = b""
    end = time.time() + 20
    while time.time() < end:
        chunk = ser.read(65536)
        if chunk:
            buf += chunk
    print("=== after launch (%d bytes) ===" % len(buf))
    print(buf.decode("utf-8", errors="replace")[-1200:])
    # send one key
    ser.write(b"j\r\n")
    end = time.time() + 15
    k = b""
    while time.time() < end:
        chunk = ser.read(65536)
        if chunk:
            k += chunk
    print("=== after j (%d bytes) ===" % len(k))
    print(k.decode("utf-8", errors="replace")[-1500:])
    for p in PANIC:
        if p in buf + k:
            print("PANIC MARKER:", p)
    ser.close()


main()
