#!/usr/bin/env python3
"""editor_large_test.py - HW verification for large-file `edit` support.

Generates a multi-line file, uploads it with the ACK-paced `receive` path
(`push_sd.push_file`), then loads it in the editor, jumps to the end, appends a
marker, saves, quits, and verifies the marker by streaming `type` back and
searching for it. This exercises the PSRAM document, the virtualized render
window (> editor_render_rows rows), and the internal DMA bounce buffer used by
save (PSRAM is not DMA-capable on this P4 build).

Usage: python tools/editor_large_test.py [COMx] [lines]
"""
import os
import sys
import time

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)))
APPS = os.path.join(os.path.dirname(TOOLS), "apps", "companion")
sys.path.insert(0, TOOLS)
sys.path.insert(0, APPS)
import push_sd  # noqa: E402
from shell_session import open_port  # noqa: E402

NAME = "EDITBIG.TXT"
MARK = "ZZENDBIGMARK"


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    lines = int(sys.argv[2]) if len(sys.argv) > 2 else 4000
    data = b"".join(b"L%05d: padding text for the large-file editor test\n" % i
                    for i in range(lines))
    print("file: %s (%d bytes, %d lines)" % (NAME, len(data), lines), flush=True)

    ser = open_port(port, 115200, 1)
    ser.reset_input_buffer()
    try:
        if not push_sd.wait_shell(ser):
            print("FAIL: shell not responding")
            return 1
        time.sleep(18.0)  # let boot background work quiesce
        ser.reset_input_buffer()
        if not push_sd.push_file(ser, NAME, data):
            print("FAIL: push")
            return 1

        def send(cmd, wait):
            ser.write((cmd + "\r\n").encode())
            time.sleep(wait)

        send("edit " + NAME, 12)
        send("\\g", 2)
        send(str(lines), 5)
        send(MARK, 2)
        send("\\s", 10)
        send("\\q", 5)
        send("y", 3)

        # Stream the file back and look for the marker on the last line.
        ser.reset_input_buffer()
        ser.write(("type " + NAME + "\r\n").encode())
        out = b""
        end = time.time() + 30
        while time.time() < end:
            chunk = ser.read(65536)
            if chunk:
                out += chunk
                if MARK.encode() in out:
                    break
        send("del " + NAME, 1.5)
    finally:
        ser.close()

    ok = MARK.encode() in out
    print("marker persisted:", ok)
    print("\nRESULT %s" % ("OK" if ok else "FAIL"))
    return 0 if ok else 1


sys.exit(main())
