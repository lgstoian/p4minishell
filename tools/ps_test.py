#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""ps_test.py - grab ps output for stack sizing.

Usage: python ps_test.py [COMx]
"""
import sys
import time

sys.path.insert(0, "tools")
from shell_session import Shell


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    sh = Shell(port)
    time.sleep(12)
    sh.s.reset_input_buffer()
    out = sh.run("ps", timeout=25)
    print(out[-3000:])
    sh.close()


main()
