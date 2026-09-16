#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""tcmd_probe.py - interactive TCMD list probe with full log.

Usage: python tcmd_probe.py [COMx]
"""
import sys
import time

sys.path.insert(0, "tools")
from bg_run import boot


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    sh = boot(port)
    sh.s.timeout = 0.5
    sh.s.reset_input_buffer()
    sh.s.write(b"TCMD\r\n")
    end = time.time() + 60
    data = b""
    while time.time() < end:
        chunk = sh.s.read(65536)
        if chunk:
            data += chunk
            if b"Commander" in data and b"Quit" in data:
                break
    text = data.decode("utf-8", errors="replace")
    print("=== list shown, tail ===", flush=True)
    print(text[-1500:], flush=True)
    for key in [b"12\r\n", b"11\r\n", b"q\r\n"]:
        print("--- sending %r ---" % key, flush=True)
        sh.s.write(key)
        time.sleep(8)
        chunk = sh.s.read(65536)
        print(chunk.decode("utf-8", errors="replace")[-600:], flush=True)
        if b"M-TCMD-DONE" in chunk or b"M-TCMD-OP" in chunk:
            break
    sh.close()


main()
