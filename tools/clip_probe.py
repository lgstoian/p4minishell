#!/usr/bin/env python3
"""clip_probe.py - verify transcript slice (clip copy) after length tracking."""
import sys
import time

sys.path.insert(0, "tools")
from shell_session import open_port


def send(ser, c, w=2.5):
    ser.reset_input_buffer()
    ser.write((c + "\r\n").encode())
    time.sleep(w)
    d = b""
    while True:
        x = ser.read(65536)
        if not x:
            break
        d += x
    return d.decode("utf-8", "replace")


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    ser = open_port(port)
    time.sleep(12)
    while True:
        if not ser.read(65536):
            break
    print("A:", repr(send(ser, "echo AAA111")[-160:]))
    print("B:", repr(send(ser, "echo BBB222")[-160:]))
    print("C:", repr(send(ser, "clip copy 2")[-200:]))
    print("D:", repr(send(ser, "clip")[-400:]))
    ser.close()


main()
