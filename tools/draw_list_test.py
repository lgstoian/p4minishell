#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""draw_list_test.py - Phase 3: draw list panel + screenshot.

Usage: python draw_list_test.py [COMx]
Builds a capture, renders it with cursor/sel/top/title, screenshots.
Saves spikes/draw_list.bmp.
"""
import os
import struct
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
from shell_session import open_port

SPIKES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "spikes")


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
    print(cmd(ser, "draw hold on")[-100:])
    print(cmd(ser, "dir /o:gn /b sd:/ > _DL.txt")[-120:])
    out = cmd(ser, 'draw list 1 2 40 18 _DL.txt 7 16 /cursor:3 /sel:1,5 /title:/')
    print("draw list:", repr(out[-250:]))
    cmd(ser, "draw refresh")
    time.sleep(1.5)
    ser.reset_input_buffer()
    ser.write(b"screenshot\r\n")
    data = b""
    end = time.time() + 60
    while time.time() < end:
        chunk = ser.read(65536)
        if chunk:
            data += chunk
            i = data.find(b"BMPX")
            if i >= 0 and len(data) >= i + 8:
                size = struct.unpack("<I", data[i + 4:i + 8])[0]
                while len(data) < i + 8 + size and time.time() < end:
                    more = ser.read(65536)
                    if more:
                        data += more
                    else:
                        time.sleep(0.2)
                with open(os.path.join(SPIKES, "draw_list.bmp"), "wb") as f:
                    f.write(data[i + 8:i + 8 + size])
                print("saved")
                break
    print("usage:", repr(cmd(ser, "draw list 1 2 40")[-200:]))
    cmd(ser, "del /p _DL.txt")
    cmd(ser, "draw hold off")
    cmd(ser, "draw close")
    ser.close()


main()
