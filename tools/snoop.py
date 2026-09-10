#!/usr/bin/env python3
"""snoop.py - read current board state WITHOUT rebooting (no DTR pulse).

Usage: python snoop.py [COMx] [seconds]
Sends nothing; prints whatever arrives (panic loops, prompts, wedges).
"""
import sys
import time

sys.path.insert(0, "tools")
from shell_session import open_port


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 8
    ser = open_port(port)
    ser.timeout = 0.5
    end = time.time() + secs
    data = b""
    while time.time() < end:
        chunk = ser.read(4096)
        if chunk:
            data += chunk
    ser.close()
    print("got %d bytes:" % len(data))
    print(data.decode("utf-8", errors="replace")[-2500:])


main()
