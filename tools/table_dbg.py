#!/usr/bin/env python3
"""table_dbg.py - read the TEMP DEBUG parse dump."""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
from shell_session import open_port


def cmd(ser, text, wait=2.5):
    ser.reset_input_buffer()
    ser.write((text + "\r\n").encode())
    time.sleep(wait)
    data = b""
    while True:
        chunk = ser.read(65536)
        if not chunk:
            break
        data += chunk
    return data


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM11"
    ser = open_port(port)
    time.sleep(12)
    while True:
        chunk = ser.read(65536)
        if not chunk:
            break
    print(repr(cmd(ser, 'draw table 2 4 7 0 "A|B|C" "1|2|3"')[-700:]))
    ser.close()


main()
