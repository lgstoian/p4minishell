#!/usr/bin/env python3
"""pipe_echo_probe.py - minimal quoted-pipeargv test.

Usage: python pipe_echo_probe.py [COMx]
echo "A|B|C" (raw remainder path) vs alias store path.
"""
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
    print("ECHO:", repr(cmd(ser, 'echo "A|B|C"')[-200:]))
    print("ECHO2:", repr(cmd(ser, 'echo X"A|B|C"Y')[-200:]))
    ser.close()


main()
