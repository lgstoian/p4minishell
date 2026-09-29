#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""push_c6.py - upload the ESP32-C6 co-processor firmware to the SD card.

Pushes the per-board ESP-Hosted slave **application image** to the SD root as
``esp32c6_hosted_slave.bin`` - exactly the name ``c6ota default`` auto-loads.
A device therefore running an older C6 can update it straight from the card:

    c6ota default      # then type YES, then `reboot`

The image is board-specific (the C6 board personality differs between the
reference board and the Tab5). Build it first, one build directory per board:

    cd coprocessor/esp32c6_slave
    idf.py -B build-jc1060p470c -DC6_BOARD=jc1060p470c set-target esp32c6
    idf.py -B build-jc1060p470c -DC6_BOARD=jc1060p470c build
    idf.py -B build-m5stack_tab5 -DC6_BOARD=m5stack_tab5 set-target esp32c6
    idf.py -B build-m5stack_tab5 -DC6_BOARD=m5stack_tab5 build

The board is auto-detected from the device's ``sysinfo`` unless ``--board`` is
given, so the same command works on either board.

Usage:
    python apps/push_c6.py COM6 [--board m5stack_tab5] [--bin PATH]
"""

import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "companion"))
from push_sd import open_port, push_file, wait_shell  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SLAVE_DIR = os.path.join(ROOT, "coprocessor", "esp32c6_slave")
TARGET = "esp32c6_hosted_slave.bin"
BOARDS = ("jc1060p470c", "m5stack_tab5")


def default_bin(board):
    """Per-board slave app image produced by the coprocessor build."""
    return os.path.join(SLAVE_DIR, "build-%s" % board, TARGET)


def probe_board(ser):
    """Read the connected board slug from `sysinfo` (board.id)."""
    ser.reset_input_buffer()
    ser.write(b"sysinfo\n")
    end = time.time() + 6.0
    buf = b""
    while time.time() < end:
        data = ser.read(ser.in_waiting or 1)
        if data:
            buf += data
            clean = re.sub(rb"\x1b\[[0-9;]*m", b"", buf)
            m = re.search(rb"board\.id:\s*([A-Za-z0-9_\-]+)", clean)
            if m:
                return m.group(1).decode("ascii", "replace")
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("port", help="COMx serial port of the board")
    ap.add_argument("--board", choices=BOARDS, default=None,
                    help="board slug (auto-detected from sysinfo when omitted)")
    ap.add_argument("--bin", dest="bin_path", default=None,
                    help="explicit slave app image to push")
    args = ap.parse_args()

    ser = open_port(args.port, 115200, 1)
    ser.reset_input_buffer()

    if not wait_shell(ser):
        print("FAIL: shell not responding on %s" % args.port)
        ser.close()
        return 1

    # The open usually reboots the board (DTR transition); let boot quiesce.
    time.sleep(20.0)
    ser.reset_input_buffer()

    board = args.board
    if board is None:
        board = probe_board(ser)
        if board not in BOARDS:
            print("FAIL: could not detect board (sysinfo board.id=%r); "
                  "pass --board" % board)
            ser.close()
            return 1
    print("board: %s" % board)

    image = args.bin_path or default_bin(board)
    if not os.path.isfile(image):
        print("FAIL: C6 image not found: %s" % image)
        print("      build it first (see this script's docstring)")
        ser.close()
        return 1
    with open(image, "rb") as f:
        data = f.read()
    print("image: %s (%d bytes)" % (image, len(data)))

    pushed = False
    for attempt in range(3):
        if push_file(ser, TARGET, data):
            pushed = True
            break
        print("  retry %s (%d/3)" % (TARGET, attempt + 1))
        time.sleep(2.0)
    ser.close()
    if pushed:
        print("PUSH ok   %s -> sd:/%s" % (board, TARGET))
        print("update the C6 from the shell with: c6ota default   (then `reboot`)")
    print("PUSH", "PASS" if pushed else "FAIL")
    return 0 if pushed else 1


if __name__ == "__main__":
    sys.exit(main())
