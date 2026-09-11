#!/usr/bin/env python3
"""tcmd_shot.py - screenshot the TCMD dual-pane UI."""
import os
import struct
import sys
import time

sys.path.insert(0, "tools")
from shell_session import open_port

SPIKES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "spikes")


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    ser = open_port(port)
    time.sleep(12)
    while True:
        if not ser.read(65536):
            break
    ser.write(b"TCMD\r\n")
    time.sleep(7)
    ser.read(65536)
    # Move cursor down a couple, then ask TCMD itself to snapshot to SD
    # (`p` = snap): the shell can't run `screenshot` while choice waits.
    ser.write(b"j\r\n")
    time.sleep(6)
    ser.read(65536)
    ser.write(b"j\r\n")
    time.sleep(6)
    ser.read(65536)
    ser.write(b"p\r\n")
    time.sleep(8)
    ser.read(65536)
    ser.write(b"q\r\n")
    time.sleep(4)
    ser.close()
    import subprocess
    subprocess.run([sys.executable, "tools/pull.py", "TCMD.BMP",
                    os.path.join(SPIKES, "tcmd.bmp"), port],
                   timeout=180)
    print("pulled")


main()
