#!/usr/bin/env python3
"""pipe_arg_probe.py - what argv does a quoted-pipe arg produce?

Usage: python pipe_arg_probe.py [COMx]
alias x="A|B|C" then alias x reveals the stored value verbatim.
Also tries for-in-set echo and choice message with pipes.
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
    time.sleep(1)
    print("ALIAS:", repr(cmd(ser, 'alias px="A|B|C"')[-200:]))
    print("SHOW:", repr(cmd(ser, "alias px")[-200:]))
    print("FOR:", repr(cmd(ser, 'for %v in ("A|B|C") do echo V=[%v]')[-300:]))
    print("CLEANUP:", repr(cmd(ser, "alias px=")[-120:]))
    ser.close()


main()
