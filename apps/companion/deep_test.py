#!/usr/bin/env python3
"""deep_test.py - reactive on-board test of the P4 Companion app.

A state-machine driver: it reads the serial stream continuously and reacts to
prompt patterns the instant they appear, so the key-wait races that plague
fixed-pacing drivers are avoided. The only fixed-delay inputs are the `ask`
modal values, which have no serial prompt.

Usage: python deep_test.py [COMx]
"""

import re
import serial
import subprocess
import sys
import time

PANIC_MARKERS = ("Guru Meditation", "assert failed", "Stack protection", "Backtrace", "Rebooting")
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
    for _ in range(15):
        ser.reset_input_buffer()
        ser.write(b"echo ready\n")
        if "ready" in read_all(ser, 2.0):
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
            return True
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


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM11"
    ser = serial.Serial(port, 115200, timeout=1)
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
    print("===== COMPANION menu walk =====")
    if not hw_reset(ser, port):
        results["menu"] = False
    else:
        log = []
        heads = ["LIVE SYSTEM DASHBOARD", "FILE TOOLS", "NETWORK TOOLS", "FUN / DEMOS", "SETTINGS"]
        got_heads = set()
        state = ["main"]          # "main" or "in_FILES"/"in_NET"/"in_FUN"/"in_SET"
        ser.reset_input_buffer()
        ser.write(b"COMPANION.BAT\n")

        def enter_module(opt):
            got_heads.add(heads[opt - 1])
            state[0] = "in_" + heads[opt - 1]

        def on_main_menu():
            if len(got_heads) >= len(heads):
                ser.write(b"6\n")          # all modules visited -> Exit
            else:
                for i, h in enumerate(heads):
                    if h not in got_heads:
                        ser.write((str(i + 1) + "\n").encode())
                        enter_module(i + 1)
                        break

        def on_module_menu():
            # Inside a module: back out to the main menu.
            if state[0].startswith("in_"):
                ser.write(b"6\n")
                state[0] = "main"

        end = time.time() + 120
        buf = ""
        exited = False
        while time.time() < end and not exited:
            data = ser.read(ser.in_waiting or 1)
            if data:
                s = data.decode(errors="replace")
                buf += s
                log.append(s)
                if "What would you like to do?" in buf:
                    state[0] = "main"
                for i, h in enumerate(heads):
                    if h in buf:
                        idx = buf.find(h)
                        buf = buf[idx + len(h):]
                        got_heads.add(h)
                        state[0] = "in_" + h
                if "Refresh (R) or Back (B)" in buf:
                    idx = buf.find("Refresh (R) or Back (B)")
                    buf = buf[idx + len("Refresh (R) or Back (B)"):]
                    ser.write(b"B\n")      # SYS back -> main menu
                    state[0] = "main"
                if "Enter choice (1-5):" in buf and state[0] == "in_FUN / DEMOS":
                    idx = buf.find("Enter choice (1-5):")
                    buf = buf[idx + len("Enter choice (1-5):"):]
                    ser.write(b"5\n")      # FUN back -> main menu
                    state[0] = "main"
                if "Enter choice (1-6):" in buf:
                    idx = buf.find("Enter choice (1-6):")
                    buf = buf[idx + len("Enter choice (1-6):"):]
                    if state[0] == "main":
                        on_main_menu()
                    else:
                        on_module_menu()
                if "Thanks for using P4 Companion" in buf:
                    exited = True
            else:
                time.sleep(0.02)
        log.append(read_all(ser, 2.0))
        blob = "".join(log)
        results["menu"] = len(got_heads) == 5 and exited and check(blob, "menu")
        print("  menu:", "PASS" if results["menu"] else "FAIL", "heads:", sorted(got_heads))
        log.append(read_all(ser, 2.0))
        blob = "".join(log)
        results["menu"] = len(got_heads) == 5 and exited and check(blob, "menu")
        print("  menu:", "PASS" if results["menu"] else "FAIL", "heads:", sorted(got_heads))

    # ---- SYS deep ----
    print("===== SYS deep =====")
    if not hw_reset(ser, port):
        results["sys"] = False
    else:
        log = []
        ser.reset_input_buffer()
        ser.write(b"call SYS.BAT::main\n")
        rc = [0]

        def on_choice(_):
            rc[0] += 1
            if rc[0] == 1:
                ser.write(b"R\n")
            else:
                ser.write(b"B\n")

        react(ser, log, [("Refresh (R) or Back (B)", on_choice)], 60)
        log.append(read_all(ser, 1.0))
        blob = "".join(log)
        results["sys"] = "LIVE SYSTEM DASHBOARD" in blob and check(blob, "sys")
        print("  sys:", "PASS" if results["sys"] else "FAIL")

    # ---- FILES deep ----
    print("===== FILES deep =====")
    if not hw_reset(ser, port):
        results["files"] = False
    else:
        log = []
        ser.reset_input_buffer()
        ser.write(b"call FILES.BAT::main\n")
        menu6 = [1, 2, 3, 4, 5, 6]
        mi = [0]
        clip_menu = [1, 3]
        ci = [0]
        notes_menu = [1, 2, 3, 4]
        ni = [0]

        def on_menu6(_):
            if mi[0] < len(menu6):
                ser.write((str(menu6[mi[0]]) + "\n").encode())
                mi[0] += 1
            else:
                ser.write(b"6\n")  # back to shell

        def on_search(_):
            ser.write(b"*.zzz\n")

        def on_trash(_):
            ser.write(b"\n")

        def on_clipmenu(_):
            ser.write((str(clip_menu[ci[0]]) + "\n").encode())
            ci[0] += 1

        def on_notesmenu(_):
            choice = notes_menu[ni[0]]
            ni[0] += 1
            ser.write((str(choice) + "\n").encode())
            if choice == 1:      # New note: two ask modals
                time.sleep(1.3)
                ser.write(b"deepnote\n")
                time.sleep(1.3)
                ser.write(b"deep body\n")
            elif choice == 3:    # View note: one ask modal
                time.sleep(1.3)
                ser.write(b"deepnote\n")

        def on_pause(_):
            ser.write(b"\n")

        rules = [
            ("Enter choice (1-6):", on_menu6),
            ("Search pattern (e.g. *.txt):", on_search),
            ("Restore entry number (blank = back):", on_trash),
            ("Enter choice (1-3):", on_clipmenu),
            ("Enter choice (1-4):", on_notesmenu),
            ("Press any key to continue", on_pause),
        ]
        react(ser, log, rules, 150)
        log.append(read_all(ser, 1.0))
        blob = "".join(log)
        results["files"] = ("Saved NOTES/deepnote.txt" in blob) and ("deep body" in blob) and check(blob, "files")
        print("  files:", "PASS" if results["files"] else "FAIL")

    # ---- NET deep (offline) ----
    print("===== NET deep (offline) =====")
    if not hw_reset(ser, port):
        results["net"] = False
    else:
        log = []
        ser.reset_input_buffer()
        ser.write(b"call NET.BAT::main\n")
        menu6 = [1, 2, 3, 5, 6]
        mi = [0]

        def on_menu6(_):
            if mi[0] < len(menu6):
                ser.write((str(menu6[mi[0]]) + "\n").encode())
                mi[0] += 1
            else:
                ser.write(b"6\n")

        def on_ssid(_):
            ser.write(b"\n")

        def on_pause(_):
            ser.write(b"\n")

        rules = [
            ("Enter choice (1-6):", on_menu6),
            ("SSID:", on_ssid),
            ("Press any key to continue", on_pause),
        ]
        react(ser, log, rules, 90)
        log.append(read_all(ser, 1.0))
        blob = "".join(log)
        results["net"] = ("Wi-Fi Status" in blob) and ("offline" in blob) and ("Fetching" in blob) and check(blob, "net")
        print("  net:", "PASS" if results["net"] else "FAIL")

    # ---- FUN deep ----
    print("===== FUN deep =====")
    if not hw_reset(ser, port):
        results["fun"] = False
    else:
        log = []
        ser.reset_input_buffer()
        ser.write(b"call FUN.BAT::main\n")
        menu5 = [1, 2, 3, 5]   # melody, rgb, guess, back
        mi = [0]
        guesses = iter([50, 25, 75, 12, 63, 37, 43, 56, 48, 52])
        phase = ["fun"]        # "fun" menu or "rgb" submenu

        def on_choice(_):
            if phase[0] == "rgb":
                phase[0] = "fun"       # after rainbow the app returns to the FUN menu
                ser.write(b"1\n")      # rainbow
            elif mi[0] < len(menu5):
                ser.write((str(menu5[mi[0]]) + "\n").encode())
                mi[0] += 1
            else:
                ser.write(b"5\n")      # back to shell

        def on_rgb_heading(_):
            phase[0] = "rgb"

        def on_guess(_):
            ser.write((str(next(guesses)) + "\n").encode())

        def on_pause(_):
            ser.write(b"\n")

        rules = [
            ("Enter choice (1-5):", on_choice),
            ("RGB LED (WS2812)", on_rgb_heading),
            ("Guess:", on_guess),
            ("Press any key to continue", on_pause),
        ]
        buf = react(ser, log, rules, 130, done=lambda b: "Correct!" in b or "Out of tries!" in b)
        log.append(read_all(ser, 1.0))
        blob = "".join(log)
        results["fun"] = ("Done." in blob) and (("Correct!" in blob) or ("Out of tries!" in blob)) and check(blob, "fun")
        if not results["fun"]:
            print("  fun tail:\n%s" % blob[-800:])
        print("  fun:", "PASS" if results["fun"] else "FAIL")

    # ---- SET deep + persistence ----
    print("===== SET deep =====")
    if not hw_reset(ser, port):
        results["set"] = False
        results["persist"] = False
    else:
        log = []
        ser.reset_input_buffer()
        ser.write(b"call SET.BAT::main\n")
        menu6 = [1, 4, 3, 6]   # brightness, scoping, show, back
        mi = [0]

        def on_menu6(_):
            if mi[0] < len(menu6):
                ser.write((str(menu6[mi[0]]) + "\n").encode())
                mi[0] += 1
            else:
                ser.write(b"6\n")

        def on_brightness(_):
            ser.write(b"63\n")

        def on_pause(_):
            ser.write(b"\n")

        rules = [
            ("Enter choice (1-6):", on_menu6),
            ("Brightness (0-100):", on_brightness),
            ("Press any key to continue", on_pause),
        ]
        react(ser, log, rules, 90)
        log.append(read_all(ser, 1.0))
        blob = "".join(log)
        results["set"] = ("Saved brightness=63" in blob) and ("brightness=63" in blob) and ("should be empty" in blob) and check(blob, "set")
        print("  set:", "PASS" if results["set"] else "FAIL")
        if hw_reset(ser, port):
            time.sleep(2.0)          # let boot settle
            read_all(ser, 4.0)       # drain boot/AUTOEXEC output
            shell_up(ser)            # confirm the shell is idle
            time.sleep(0.5)
            read_all(ser, 1.0)       # drain residual
            ser.reset_input_buffer()
            ser.write(b"appconfig companion\n")
            p = ansi_strip(read_all(ser, 4.0))
            results["persist"] = "brightness=63" in p
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