# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Map P4MiniShell board profiles to COM ports so two boards can be flashed and
tested side by side.

Both the reference board and the M5Stack Tab5 enumerate as the same USB
USB-Serial-JTAG VID/PID, so the COM number alone cannot identify a board and can
change between replugs. Two stable identifiers are used:

1. The board firmware's ``sysinfo`` prints ``board.id: <slug>`` (the profile
   slug from ``board_config.h``: ``jc1060p470c`` / ``m5stack_tab5``). This is
   authoritative once the board runs firmware.
2. Each board's USB serial number (the chip MAC) is fixed. A small JSON map
   (``tools/board_ports.json``) records ``slug -> usb_serial`` so an unflashed or
   unresponsive board can still be resolved.

Usage::

    python tools/board_ports.py list                 # show ports + ids
    python tools/board_ports.py auto                 # learn responsive boards
    python tools/board_ports.py set m5stack_tab5 COM6
    python tools/board_ports.py get m5stack_tab5     # -> COM6

The mapping file is machine-local and git-ignored. ``P4_BOARD=<slug>`` selects a
board for the existing tools (see tools/shell_session.default_port).
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import serial  # noqa: E402
import serial.tools.list_ports as list_ports  # noqa: E402

MAP_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "board_ports.json")

ANSI_RE = re.compile(rb"\x1b\[[0-9;]*m")
BOARD_ID_RE = re.compile(rb"board\.id:\s*([A-Za-z0-9_\-]+)")


def _load_map() -> dict:
    try:
        with open(MAP_PATH, "r", encoding="utf-8") as fh:
            data = json.load(fh)
            return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def _save_map(mapping: dict) -> None:
    with open(MAP_PATH, "w", encoding="utf-8") as fh:
        json.dump(mapping, fh, indent=2, sort_keys=True)
        fh.write("\n")


def _port_info(device: str):
    for p in list_ports.comports():
        if p.device.upper() == device.upper():
            return p
    return None


def _probe_board_id(port: str, timeout: float = 4.0):
    """Open the port DTR-safe and read the board slug from `sysinfo`.

    Returns None when the board does not answer (unflashed, crashed, or another
    program holds the port).
    """
    try:
        ser = serial.Serial()
        ser.port = port
        ser.baudrate = 115200
        ser.timeout = 0.25
        try:
            ser.dtr = False
            ser.rts = False
        except Exception:
            pass
        ser.open()
    except Exception:
        return None

    try:
        ser.reset_input_buffer()
        ser.write(b"\r\nsysinfo\r\n")
        deadline = time.time() + timeout
        buf = b""
        while time.time() < deadline:
            chunk = ser.read(4096)
            if chunk:
                buf += chunk
                # The transcript colours the label, so strip SGR codes before
                # matching or the escape bytes between `board.id:` and the
                # slug break the pattern.
                m = BOARD_ID_RE.search(ANSI_RE.sub(b"", buf))
                if m:
                    return m.group(1).decode("ascii", "replace")
        return None
    except Exception:
        return None
    finally:
        try:
            ser.close()
        except Exception:
            pass


def _candidate_ports():
    return [p.device for p in list_ports.comports()]


def cmd_list(args):
    mapping = _load_map()
    serial_to_board = {v: k for k, v in mapping.items() if v}
    print("port  usb_serial                 board_id (probe)      mapped")
    for device in _candidate_ports():
        info = _port_info(device)
        usb = (info.serial_number if info else None) or "-"
        probe = _probe_board_id(device) if args.probe else None
        mapped = serial_to_board.get(usb, "-")
        print("%-5s %-26s %-21s %s" % (device, usb, probe or "-", mapped))
    return 0


def cmd_auto(args):
    mapping = _load_map()
    learned = 0
    for device in _candidate_ports():
        info = _port_info(device)
        usb = info.serial_number if info else None
        board = _probe_board_id(device)
        if board and usb:
            mapping[board] = usb
            learned += 1
            print("learned %s -> %s (usb %s)" % (board, device, usb))
    if learned:
        _save_map(mapping)
    print("updated %d board(s); map: %s" % (learned, MAP_PATH))
    return 0


def cmd_set(args):
    info = _port_info(args.port)
    if info is None:
        print("error: port %s not present" % args.port, file=sys.stderr)
        return 2
    usb = info.serial_number
    if not usb:
        print("error: port %s has no USB serial number" % args.port, file=sys.stderr)
        return 2
    mapping = _load_map()
    mapping[args.board] = usb
    _save_map(mapping)
    print("bound %s -> %s (usb %s)" % (args.board, args.port, usb))
    return 0


def resolve_port(board: str):
    """Return the COM port for a board slug, or None.

    Prefers the live USB serial binding; falls back to a live probe so a board
    only needs to be flashed once for resolution to keep working.
    """
    mapping = _load_map()
    want = mapping.get(board)
    if want:
        for p in list_ports.comports():
            if p.serial_number == want:
                return p.device
    # No/lost binding: probe every port for the slug.
    for p in list_ports.comports():
        if _probe_board_id(p.device) == board:
            if p.serial_number and mapping.get(board) != p.serial_number:
                mapping[board] = p.serial_number
                _save_map(mapping)
            return p.device
    return None


def cmd_get(args):
    port = resolve_port(args.board)
    if port is None:
        print("error: no port resolved for board '%s' (see 'list'/'auto'/'set')"
              % args.board, file=sys.stderr)
        return 1
    # Machine-readable: only the port on stdout.
    print(port)
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    p_list = sub.add_parser("list", help="list ports with detected board ids")
    p_list.add_argument("--probe", action="store_true",
                        help="also send sysinfo to each port (waits for the prompt)")
    p_list.set_defaults(func=cmd_list)

    p_auto = sub.add_parser("auto", help="probe ports and learn responsive board ids")
    p_auto.set_defaults(func=cmd_auto)

    p_set = sub.add_parser("set", help="bind a board slug to a COM port")
    p_set.add_argument("board")
    p_set.add_argument("port")
    p_set.set_defaults(func=cmd_set)

    p_get = sub.add_parser("get", help="print the COM port for a board slug")
    p_get.add_argument("board")
    p_get.set_defaults(func=cmd_get)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
