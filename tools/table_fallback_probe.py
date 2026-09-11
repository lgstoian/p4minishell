#!/usr/bin/env python3
"""table_fallback_probe.py - force the off-TUI table path, read parse result.

Usage: python table_fallback_probe.py [COMx]
draw close (ignore error), then draw table -> off-TUI bordered block shows
exactly what the verb parsed. Raw repr, no stripping.
"""
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
from shell_session import open_port


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM11"
    ser = open_port(port)
    time.sleep(1)
    ser.reset_input_buffer()
    ser.write(b"draw close\r\n")
    time.sleep(2.0)
    ser.read(65536)
    line = 'draw table 2 4 7 0 "A|B|C" "1|2|3" "4|5|6"'
    ser.write((line + "\r\n").encode())
    time.sleep(3.0)
    print(repr(ser.read(65536)))
    ser.close()


main()
