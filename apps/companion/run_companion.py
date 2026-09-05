#!/usr/bin/env python3
"""run_companion.py - drive the P4 Companion app on the board and check it.

Each scenario runs a command, feeds scripted inputs (triggered on a marker in
the transcript, with pauses accounted for), captures the transcript, and
asserts expected strings.

Usage:
    python run_companion.py [COMx] <scenario>
"""

import serial
import sys
import time

PORT = "COM11"


def read_all(ser, seconds):
    end = time.time() + seconds
    out = []
    while time.time() < end:
        data = ser.read(ser.in_waiting or 1)
        if data:
            out.append(data.decode(errors="replace"))
    return "".join(out)


def wait_for(ser, needle, timeout=12.0):
    buf = ""
    end = time.time() + timeout
    while time.time() < end:
        d = read_all(ser, 0.3)
        if d:
            buf += d
            if needle in buf:
                return buf
    return buf


def cmd(ser, text, read_after=2.0):
    ser.reset_input_buffer()
    ser.write((text + "\n").encode())
    return read_all(ser, read_after)


# A feed step: (trigger_substring_or_None, input, delay_after_trigger_ms).
# Menu choices wait for the actual "Enter choice" prompt; modal inputs (ask,
# set /p) have no transcript prompt, so they wait the fixed delay after the
# previous step. "press" steps send a bare Enter to dismiss a `pause`.
SCENARIOS = {
    "sys": {
        "cmd": "call SYS.BAT::main",
        "feeds": [
            ("Refresh (R) or Back (B)", "B", 400),
        ],
        "expect": ["LIVE SYSTEM DASHBOARD", "mem.heap", "battery"],
    },
    "files_notes": {
        "cmd": "call FILES.BAT::main",
        "feeds": [
            ("Enter choice (1-6):", "5", 700),   # note pad
            ("Enter choice (1-4):", "1", 900),   # new note
            (None, "t1", 1200),                  # ask: note name
            (None, "hello body", 1200),          # ask: body
            (None, "4", 900),                    # dismiss pause -> notes menu
            ("Enter choice (1-4):", "4", 700),   # back -> files menu
            ("Enter choice (1-6):", "6", 700),   # back -> shell
        ],
        "expect": ["Saved NOTES/t1.txt"],
    },
    "set": {
        "cmd": "call SET.BAT::main",
        "feeds": [
            ("Enter choice (1-6):", "1", 700),   # set brightness
            ("Brightness (0-100)", "55", 700),
            (None, "3", 900),                    # dismiss pause -> settings menu
            ("Enter choice (1-6):", "6", 700),   # back -> shell
        ],
        "expect": ["Saved brightness=55"],
    },
    "full": {
        "cmd": "COMPANION.BAT",
        "feeds": [
            ("Enter choice (1-6):", "1", 500),   # Live System
            ("Refresh (R) or Back (B)", "B", 500),
            ("Enter choice (1-6):", "6", 500),   # Exit
        ],
        "expect": ["LIVE SYSTEM DASHBOARD", "Thanks for using P4 Companion"],
    },
    "net_offline": {
        "cmd": "call NET.BAT::main",
        "feeds": [
            ("Enter choice (1-6):", "1", 700),   # wifi status
            (None, "3", 900),                    # dismiss pause -> ping
            (None, "5", 900),                    # dismiss pause -> joke
            (None, "6", 900),                    # dismiss pause -> back
        ],
        "expect": ["NETWORK TOOLS", "Wi-Fi Status"],
    },
}


def run_scenario(ser, cfg):
    log = ""
    ser.reset_input_buffer()
    ser.write((cfg["cmd"] + "\n").encode())
    # Flush the first chunk so a stale transcript backlog (e.g. an appmode
    # screen restore) can't satisfy an early trigger. Everything lands in the
    # shared `log` so a trigger that renders during the flush is still found.
    log += read_all(ser, 1.0)
    for (trigger, inp, delay_ms) in cfg["feeds"]:
        if trigger is not None:
            deadline = time.time() + 15
            while trigger not in log and time.time() < deadline:
                log += read_all(ser, 0.3)
            if trigger not in log:
                print("  !! trigger never seen: %r" % trigger)
        else:
            log += read_all(ser, 0.2)
        time.sleep(delay_ms / 1000.0)
        if inp is not None:
            ser.write((inp + "\n").encode())
            print("  fed %r" % inp)
        log += read_all(ser, 0.3)
    log += read_all(ser, 1.5)
    return log


def main():
    args = sys.argv[1:]
    port = args[0] if args and args[0].startswith("COM") else PORT
    scenario = args[1] if len(args) > 1 else "full"
    if scenario not in SCENARIOS:
        print("unknown scenario %r; choose from %s" % (scenario, list(SCENARIOS)))
        return 1

    ser = serial.Serial(port, 115200, timeout=1)
    time.sleep(0.5)
    ser.reset_input_buffer()
    for _ in range(10):
        ser.reset_input_buffer()
        ser.write(b"echo ready\n")
        if "ready" in read_all(ser, 2.0):
            break
        time.sleep(1.5)

    # Clear the transcript so stale backlog cannot satisfy menu triggers, and
    # let the shell settle before the scenario starts.
    ser.write(b"cls\n")
    time.sleep(1.0)
    ser.reset_input_buffer()

    cfg = SCENARIOS[scenario]
    print("===== SCENARIO: %s (cmd=%s) =====" % (scenario, cfg["cmd"]))
    log = run_scenario(ser, cfg)
    ser.close()

    ok = True
    for exp in cfg["expect"]:
        if exp in log:
            print("PASS expect %r" % exp)
        else:
            print("FAIL expect %r" % exp)
            ok = False
    for marker in ("assert failed", "Guru Meditation", "Stack protection", "Backtrace"):
        if marker in log:
            print("FAIL panic marker %r" % marker)
            ok = False
    if not ok:
        print("----- LOG (tail 2000) -----")
        print(log[-2000:])
    print("SCENARIO", scenario, "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
