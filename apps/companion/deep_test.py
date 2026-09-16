#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""deep_test.py - reactive on-board test of the P4 Companion app.

A state-machine driver: it reads the serial stream continuously and reacts to
prompt patterns the instant they appear, so the key-wait races that plague
fixed-pacing drivers are avoided. The only fixed-delay inputs are the `ask`
modal values, which have no serial prompt.

Usage: python deep_test.py [COMx]
"""

import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "tools"))
from shell_session import open_port  # noqa: E402

PANIC_MARKERS = ("Guru Meditation", "assert failed", "Stack protection", "Backtrace", "Rebooting")
ANSI = re.compile(r"\x1b\[[0-9;]*m")


def worker_line(text, word):
    """True only if the worker actually printed `word` as its own output
    line. The submit-side echo (`PS \\> echo WORD`) contains the word too,
    so substring matching false-passes on a wedged worker. One leading
    `PS ...>` prompt prefix is stripped first: prompt-glue (trailing prompt
    + output on one serial line, normal under load) must not false-negative."""
    for ln in text.splitlines():
        s = ANSI.sub("", ln).strip()
        s = re.sub(r"^PS \S*> ", "", s).strip()
        if s == word and ("echo %s" % word) not in ANSI.sub("", ln):
            return True
    return False
UPTIME_RE = re.compile(r"uptime=(\d+)d (\d+)h (\d+)m (\d+)s")
ESPTool = r"C:\esp\v5.5.5\esp-idf\components\esptool_py\esptool\esptool.py"


def read_all(ser, seconds):
    end = time.time() + seconds
    out = []
    while time.time() < end:
        data = ser.read(ser.in_waiting or 1)
        if data:
            out.append(data.decode(errors="replace"))
    return "".join(out)


def shell_up(ser):
    # Requires the worker's own `ready` output line, not the `echo ready`
    # input echo (which the console task prints even while wedged).
    for _ in range(15):
        ser.reset_input_buffer()
        ser.write(b"echo ready\n")
        if worker_line(read_all(ser, 2.0), "ready"):
            return True
        time.sleep(1)
    return False


def uptime(ser):
    ser.reset_input_buffer()
    ser.write(b"sysinfo\n")
    b = read_all(ser, 3.5)
    m = UPTIME_RE.search(b)
    if not m:
        return None
    return (int(m.group(1)) * 86400 + int(m.group(2)) * 3600 +
            int(m.group(3)) * 60 + int(m.group(4)))


def sd_mounted(ser):
    """The shared-SDMMC N1 race wedges SD+WiFi on ~1/15 boots; never start a
    section on such a boot (every SD verb would fail spuriously)."""
    ser.reset_input_buffer()
    ser.write(b"sd info\n")
    b = read_all(ser, 4.0)
    return ("fs_total" in b or "MSSD0" in b) and "No SD card" not in b


def esptool_hard_reset(port):
    try:
        subprocess.run(
            [sys.executable, ESPTool, "--chip", "esp32p4", "-p", port, "-b", "460800",
             "--before=default_reset", "--after=hard_reset", "read_mac"],
            capture_output=True, timeout=60)
        return True
    except Exception as exc:
        print("  esptool reset failed: %r" % exc)
        return False


def open_noreset(port, baud=115200, timeout=1):
    """Open the USB-Serial/JTAG port WITHOUT rebooting the board (the shared
    shell_session.open_port pre-sets DTR/RTS before open so the reset never
    fires)."""
    return open_port(port, baud, timeout)


def hw_reset(ser, port):
    for attempt in range(4):
        try:
            ser.close()
        except Exception:
            pass
        esptool_hard_reset(port)
        time.sleep(1.0)
        try:
            ser.open()
            ser.setDTR(False)
            ser.setRTS(False)
            time.sleep(0.5)
        except Exception:
            print("  reset attempt %d: reopen failed" % (attempt + 1))
            time.sleep(2)
            continue
        if not shell_up(ser):
            ser.setDTR(False)
            ser.setRTS(True)
            time.sleep(0.1)
            ser.setRTS(False)
            time.sleep(2.0)
            ser.reset_input_buffer()
            if not shell_up(ser):
                print("  reset attempt %d: no shell" % (attempt + 1))
                time.sleep(1)
                continue
        u = uptime(ser)
        if u is not None and u < 45:
            if sd_mounted(ser):
                return True
            print("  reset attempt %d: SD wedged (N1), retrying" % (attempt + 1))
        else:
            print("  reset attempt %d: uptime %s (stale, retrying)" % (attempt + 1, u))
        time.sleep(1)
    return False


def check(blob, name):
    bad = [m for m in PANIC_MARKERS if m in blob]
    if bad:
        print("  %s: PANIC MARKER: %s" % (name, bad))
        return False
    return True


ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")


def ansi_strip(text):
    return ANSI_RE.sub("", text)


def react(ser, log, rules, duration, done=None):
    """Read serial until `duration` elapses, firing `rules` (pattern, handler)
    on every occurrence. Handlers feed input. `done(buf)` can end early.
    Returns the leftover buffer."""
    buf = ""
    end = time.time() + duration
    while time.time() < end:
        data = ser.read(ser.in_waiting or 1)
        if data:
            s = data.decode(errors="replace")
            buf += s
            log.append(s)
            for pattern, handler in rules:
                while pattern in buf:
                    idx = buf.find(pattern)
                    buf = buf[idx + len(pattern):]
                    handler(pattern)
            if done and done(buf):
                return buf
        else:
            time.sleep(0.02)
    return buf


def cmd(ser, text, read_after=2.0):
    ser.reset_input_buffer()
    ser.write((text + "\n").encode())
    return read_all(ser, read_after)


def send_after_settle(ser, text, settle=10.0):
    """Send one serial line after a settle delay. Markers echo BEFORE the
    draws that open the modal, and the first modal after boot can take tens
    of seconds (post-boot background activity); the settle covers the open
    latency. Single sends only: a blind second send can land in the newly
    opened menu and activate a random item."""
    time.sleep(settle)
    ser.write(text)
    time.sleep(1.0)


def press(ser, st, text, expect, timeout=45.0):
    """Send once (settled); retry once if the expected marker never shows
    (the first may have predated the modal open). Covers opens up to ~100s
    late; the steady state answers in seconds."""
    send_after_settle(ser, text)
    if st.wait_for(expect, timeout):
        return True
    ser.write(text)   # one recovery retry
    time.sleep(1.0)
    return st.wait_for(expect, timeout)


class Stream:
    """Serial reader with a persistent buffer. wait_for() consumes through
    the match and KEEPS the tail: markers that arrive early (during a
    feed_twice settle) must survive for the next wait instead of being
    swallowed with the match."""

    def __init__(self, ser):
        self.ser = ser
        self.buf = ""
        self.log = []

    def _pump(self):
        data = self.ser.read(self.ser.in_waiting or 1)
        if data:
            s = data.decode(errors="replace")
            self.buf += s
            self.log.append(s)
            return True
        return False

    def wait_for(self, marker, timeout=30.0):
        end = time.time() + timeout
        while time.time() < end:
            if marker in self.buf:
                idx = self.buf.find(marker) + len(marker)
                self.buf = self.buf[idx:]
                return True
            if not self._pump():
                time.sleep(0.02)
        return False


def wait_for(ser, log, marker, timeout=30.0):
    """One-shot wait (no tail kept): only for terminal markers after which
    the next step re-syncs with reset_input_buffer + fixed reads."""
    end = time.time() + timeout
    buf = ""
    while time.time() < end:
        data = ser.read(ser.in_waiting or 1)
        if data:
            s = data.decode(errors="replace")
            buf += s
            log.append(s)
            if marker in buf:
                return True
        else:
            time.sleep(0.02)
    return False


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM11"
    ser = open_noreset(port)
    time.sleep(0.5)
    ser.reset_input_buffer()
    if not shell_up(ser):
        print("startup: no shell, attempting a hard reset")
        if not hw_reset(ser, port):
            print("FAIL: shell not responding")
            ser.close()
            return 1

    results = {}

    # ---- baseline ----
    print("===== baseline: apps / launch /list =====")
    ser.reset_input_buffer()
    ser.write(b"apps\n")
    a = ansi_strip(read_all(ser, 4.0))
    ser.write(b"launch /list\n")
    l = ansi_strip(read_all(ser, 5.0))
    results["baseline"] = "hello" in a and "COMPANION" in l and "P4 Companion" in l
    print("  baseline:", "PASS" if results["baseline"] else "FAIL")

    # ---- COMPANION main menu walk ----
    # Serial-visible echo markers drive this (modals draw only on LVGL, so
    # the driver keys on markers, never on screen text). Chained waits share
    # one Stream so an early marker survives for the next wait.
    print("===== COMPANION menu walk =====")
    if not hw_reset(ser, port):
        results["menu"] = False
    else:
        mods = [("1", "[M-SYS]", "[M-SYS-BACK]"),
                ("2", "[M-FILES]", "[M-FILES-BACK]"),
                ("3", "[M-NET]", "[M-NET-BACK]"),
                ("4", "[M-FUN]", "[M-FUN-BACK]"),
                ("5", "[M-SET]", "[M-SET-BACK]")]
        got = set()
        menu_ok = True
        ser.reset_input_buffer()
        st = Stream(ser)
        ser.write(b"COMPANION.BAT\n")
        for sel, mark, backmark in mods:
            if not st.wait_for("[C-MENU]", 40.0):
                menu_ok = False
                break
            if not press(ser, st, (sel + "\n").encode(), mark):
                menu_ok = False
                break
            got.add(mark)
            if not press(ser, st, b"q\n", backmark):  # cancel list -> :back
                menu_ok = False
                break
        if menu_ok:
            if not st.wait_for("[C-MENU]", 40.0):
                menu_ok = False
            else:
                send_after_settle(ser, b"6\n")   # Exit
                menu_ok = st.wait_for("[C-EXIT]", 30.0)
        blob = "".join(st.log)
        results["menu"] = menu_ok and len(got) == 5 and check(blob, "menu")
        print("  menu:", "PASS" if results["menu"] else "FAIL", "mods:", sorted(got))

    # ---- SYS deep ----
    print("===== SYS deep =====")
    if not hw_reset(ser, port):
        results["sys"] = False
    else:
        ser.reset_input_buffer()
        st = Stream(ser)
        ser.write(b"call SYS.BAT::main\n")
        sys_ok = st.wait_for("[M-SYS]", 30.0)
        print("  sys marker:", sys_ok, flush=True)
        if sys_ok:
            sys_ok = press(ser, st, b"3\n", "[M-SYS-SAVED]")  # Save snapshot
            print("  sys saved:", sys_ok, flush=True)
        if sys_ok:
            send_after_settle(ser, b"ok\n")      # dismiss Saved dialog
            time.sleep(2.0)
            # Services submenu: status -> lists tasks + alarms.
            sys_svc = press(ser, st, b"4\n", "[M-SYS-SVC]")
            print("  sys services:", sys_svc, flush=True)
            if sys_svc:
                sys_svc = press(ser, st, b"1\n", "[M-SVC-STATUS]")
                print("  sys svc status:", sys_svc, flush=True)
            if sys_svc:
                send_after_settle(ser, b"ok\n")  # dismiss Services dialog
                time.sleep(2.0)
                send_after_settle(ser, b"6\n")   # services Back -> dashboard
                time.sleep(2.0)
            if sys_ok:
                # Packages submenu: entering it runs `pkg list` then its menu.
                sys_ok = press(ser, st, b"5\n", "[M-SYS-PKG]")
                print("  sys packages:", sys_ok, flush=True)
                if sys_ok:
                    send_after_settle(ser, b"5\n")  # Packages Back -> dashboard
                    time.sleep(2.0)
            sys_ok = press(ser, st, b"6\n", "[M-SYS-BACK]")   # Back
            print("  sys back:", sys_ok, flush=True)
        if sys_ok:
            # Real artifact check: the snapshot must hold mem output.
            ser.reset_input_buffer()
            ser.write(b"type SYS_SNAP.TXT\n")
            snap = ansi_strip(read_all(ser, 5.0))
            st.log.append(snap)
            sys_ok = "mem.heap" in snap
        blob = "".join(st.log)
        results["sys"] = sys_ok and check(blob, "sys")
        print("  sys:", "PASS" if results["sys"] else "FAIL")

    # ---- FILES deep ----
    print("===== FILES deep =====")
    if not hw_reset(ser, port):
        results["files"] = False
    else:
        ser.reset_input_buffer()
        st = Stream(ser)
        ser.write(b"call FILES.BAT::main\n")
        files_ok = st.wait_for("[M-FILES]", 30.0)
        if files_ok:
            files_ok = press(ser, st, b"6\n", "[M-FILES-NOTES]")  # Note pad
        if files_ok:
            send_after_settle(ser, b"1\n")       # New note -> ask name
            time.sleep(4.0)                      # ask modal opens (serial-silent)
            send_after_settle(ser, b"deepnote\n", settle=3.0)
            time.sleep(4.0)                      # ask body opens
            send_after_settle(ser, b"deep body\n", settle=3.0)
            time.sleep(5.0)                      # note saved, Saved dialog opens
            send_after_settle(ser, b"ok\n", settle=3.0)  # dismiss dialog
            time.sleep(2.0)
            send_after_settle(ser, b"5\n", settle=3.0)   # notes Back
            time.sleep(2.0)
            files_ok = press(ser, st, b"9\n", "[M-FILES-BACK]")  # files Back
        if files_ok:
            # Real artifact check: the note file must hold the body.
            ser.reset_input_buffer()
            ser.write(b"type NOTES/deepnote.txt\n")
            note = ansi_strip(read_all(ser, 5.0))
            st.log.append(note)
            files_ok = "deep body" in note
        blob = "".join(st.log)
        results["files"] = files_ok and check(blob, "files")
        print("  files:", "PASS" if results["files"] else "FAIL")

    # ---- NET deep (offline) ----
    print("===== NET deep (offline) =====")
    if not hw_reset(ser, port):
        results["net"] = False
    else:
        ser.reset_input_buffer()
        st = Stream(ser)
        ser.write(b"call NET.BAT::main\n")
        net_ok = st.wait_for("[M-NET]", 30.0)
        print("  net marker:", net_ok, flush=True)
        if net_ok:
            net_ok = press(ser, st, b"6\n", "[M-NET-FETCHED]")  # Fetch joke
            print("  net fetched:", net_ok, flush=True)
        if net_ok:
            send_after_settle(ser, b"ok\n", settle=3.0)  # dismiss Fetch dialog
            time.sleep(2.0)
            # The flow may have opened the Result viewer (or be back at the
            # menu already): q closes a viewer, cancels a menu back to :main
            # (which reopens it), or is a harmless unknown command. Either
            # way the main menu ends up open for the Back selection below.
            send_after_settle(ser, b"q\n", settle=3.0)
            time.sleep(2.0)
            net_ok = press(ser, st, b"7\n", "[M-NET-BACK]")  # Back
            print("  net back:", net_ok, flush=True)
        blob = "".join(st.log)
        results["net"] = net_ok and check(blob, "net")
        print("  net:", "PASS" if results["net"] else "FAIL")

    # ---- FUN deep ----
    print("===== FUN deep =====")
    if not hw_reset(ser, port):
        results["fun"] = False
    else:
        ser.reset_input_buffer()
        st = Stream(ser)
        ser.write(b"call FUN.BAT::main\n")
        fun_ok = st.wait_for("[M-FUN]", 30.0)
        if fun_ok:
            send_after_settle(ser, b"4\n")       # Calc playground -> ask expr
            time.sleep(4.0)                      # ask modal opens (serial-silent)
            send_after_settle(ser, b"6*7\n", settle=3.0)
            time.sleep(4.0)                      # calc runs, Result viewer opens
            send_after_settle(ser, b"q\n", settle=3.0)  # close viewer
            time.sleep(2.0)
            fun_ok = press(ser, st, b"9\n", "[M-FUN-BACK]")  # Back
        if fun_ok:
            # Real computation check through the same engine.
            ser.reset_input_buffer()
            ser.write(b"calc 6*7\n")
            out = ansi_strip(read_all(ser, 5.0))
            st.log.append(out)
            fun_ok = "42" in out
        blob = "".join(st.log)
        results["fun"] = fun_ok and check(blob, "fun")
        print("  fun:", "PASS" if results["fun"] else "FAIL")

    # ---- SET deep + persistence ----
    print("===== SET deep =====")
    if not hw_reset(ser, port):
        results["set"] = False
        results["persist"] = False
    else:
        ser.reset_input_buffer()
        st = Stream(ser)
        ser.write(b"call SET.BAT::main\n")
        set_ok = st.wait_for("[M-SET]", 30.0)
        if set_ok:
            send_after_settle(ser, b"1\n")       # Set brightness -> ask value
            time.sleep(4.0)                      # ask modal opens (serial-silent)
            send_after_settle(ser, b"63\n", settle=3.0)
            time.sleep(5.0)                      # brightness set, Saved dialog opens
            send_after_settle(ser, b"ok\n", settle=3.0)  # dismiss dialog
            time.sleep(2.0)
            # Theme submenu: serial selection is 1-based (list returns the
            # 0-based index as ERRORLEVEL; serial input is index+1). Theme is
            # menu item 5 -> "6", its Back is item 5 -> "6".
            set_theme = press(ser, st, b"6\n", "[M-SET-THEME]")
            print("  set theme:", set_theme, flush=True)
            if set_theme:
                send_after_settle(ser, b"6\n")   # Theme Back -> settings
                time.sleep(2.0)
            set_ok = press(ser, st, b"13\n", "[M-SET-BACK]")  # Back (0-based 12)
            print("  set back:", set_ok, flush=True)
        if set_ok:
            # Real artifact check: persisted brightness survives the flow.
            # Settings persist in CONFIG.SYS (config /b), not companion.INI.
            ser.reset_input_buffer()
            ser.write(b"config BRIGHTNESS /b\n")
            got = ansi_strip(read_all(ser, 5.0))
            st.log.append(got)
            set_ok = "63" in got
        blob = "".join(st.log)
        results["set"] = set_ok and check(blob, "set")
        print("  set:", "PASS" if results["set"] else "FAIL")
        if hw_reset(ser, port):
            time.sleep(2.0)          # let boot settle
            read_all(ser, 4.0)       # drain boot/AUTOEXEC output
            shell_up(ser)            # confirm the shell is idle
            time.sleep(0.5)
            read_all(ser, 1.0)       # drain residual
            ser.reset_input_buffer()
            ser.write(b"config BRIGHTNESS /b\n")
            p = ansi_strip(read_all(ser, 4.0))
            results["persist"] = "63" in p
        else:
            results["persist"] = False
        print("  persist:", "PASS" if results["persist"] else "FAIL")

    ser.close()

    print("\n===== RESULTS =====")
    allok = True
    for k, v in results.items():
        print("%-10s %s" % (k, "PASS" if v else "FAIL"))
        if not v:
            allok = False
    print("DEEP", "PASS" if allok else "FAIL")
    return 0 if allok else 1


if __name__ == "__main__":
    sys.exit(main())