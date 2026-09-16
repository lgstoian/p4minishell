#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""alias_pipe_probe.py - argv-level quoted-pipe test via alias store.

Usage: python alias_pipe_probe.py [COMx]
One stable session: alias px="A|B|C", alias px, alias px= (clear).
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
    print("SET:", repr(cmd(ser, 'alias px="A|B|C"')[-200:]))
    print("SHOW:", repr(cmd(ser, "alias px")[-300:]))
    print("CLR:", repr(cmd(ser, "alias px=")[-150:]))
    ser.close()


main()
