#!/usr/bin/env python3
"""plot_test.py - HW verification for the `plot` coordinate layer.

Canvas: PLOT.BAT (axes+grid, sin/cos, world line) -> gfx save -> pull BMP ->
pixel checks; then data/bar/point/line/polar/para/table/usage stages with
their own saves+pulls. TUI: `plot tui on` + axes + func, verified via an
in-session screenshot (saved to spikes/plot_tui.bmp).

Usage: python tools/plot_test.py [COMx]
"""
import os
import struct
import subprocess
import sys
import time

sys.path.insert(0, "tools")
sys.path.insert(0, os.path.join("apps", "companion"))
from push_sd import push_file, wait_shell  # noqa: E402
from bg_run import boot, run_quiet  # noqa: E402
import serial  # noqa: E402

FAILS = []
TMP = r"C:\Users\lgstoian\AppData\Local\Temp\opencode"


def q(rgb):
    """RGB888 -> RGB565 -> RGB888 (the firmware's quantization)."""
    r = (rgb >> 16) & 0xFF
    g = (rgb >> 8) & 0xFF
    b = rgb & 0xFF
    v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    return (((v >> 11) & 0x1F) << 3, ((v >> 5) & 0x3F) << 2, (v & 0x1F) << 3)


def check(name, got, want):
    ok = got == want
    print("  [%s] %s got=%s want=%s" % ("PASS" if ok else "FAIL", name, got, want))
    if not ok:
        FAILS.append(name)


class Bmp:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        assert self.data[:2] == b"BM", "not a BMP"
        self.off = struct.unpack("<I", self.data[10:14])[0]
        self.w = struct.unpack("<i", self.data[18:22])[0]
        self.h = struct.unpack("<i", self.data[22:26])[0]
        self.stride = (self.w * 3 + 3) & ~3

    def px(self, x, y):
        """Top-down (x, y) -> (R, G, B); BMP rows are bottom-up."""
        base = self.off + (self.h - 1 - y) * self.stride + x * 3
        b, g, r = self.data[base], self.data[base + 1], self.data[base + 2]
        return (r, g, b)

    def count(self, x0, y0, x1, y1, want):
        n = 0
        for y in range(y0, y1):
            for x in range(x0, x1):
                if self.px(x, y) == want:
                    n += 1
        return n


def pull(port, remote, local):
    last = ""
    for _ in range(3):
        time.sleep(2)
        r = subprocess.run([sys.executable, "tools/pull.py", remote, local, port],
                           capture_output=True, text=True)
        if "saved" in r.stdout:
            return True
        last = "rc=%d out=%r err=%r" % (r.returncode, r.stdout[-200:], r.stderr[-200:])
    FAILS.append("pull " + remote)
    print("  [FAIL] pull %s: %s" % (remote, last))
    return False


def screenshot(sh, out):
    """In-session `screenshot` -> BMPX frame -> file. Returns True on save."""
    sh.s.reset_input_buffer()
    sh.s.write(b"screenshot\r\n")
    data = b""
    end = time.time() + 60
    while time.time() < end:
        chunk = sh.s.read(65536)
        if chunk:
            data += chunk
            i = data.find(b"BMPX")
            if i >= 0 and len(data) >= i + 8:
                size = struct.unpack("<I", data[i + 4:i + 8])[0]
                while len(data) < i + 8 + size and time.time() < end:
                    more = sh.s.read(65536)
                    if more:
                        data += more
                    else:
                        time.sleep(0.2)
                with open(out, "wb") as f:
                    f.write(data[i + 8:i + 8 + size])
                return True
    return False


DATA = b"# x,y pairs\n0,0\n5,5\n-5,-5\n"

# Bright CGA palette (components/tui/tui.c s_dos_rgb), RGB565-quantized.
GRAY = (80, 84, 80)        # q(0x555555), DOS 8
YELLOW = (248, 252, 80)    # q(0xFFFF55), DOS 14
GREEN = (80, 252, 80)      # q(0x55FF55), DOS 10
CYAN = (80, 252, 248)      # q(0x55FFFF), DOS 11
WHITE = (248, 252, 248)    # q(0xFFFFFF), DOS 15


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"

    # Push the data file on a raw serial session first (proven pattern).
    ser = serial.Serial(port, 115200, timeout=1)
    ser.setDTR(False)
    ser.setRTS(False)
    time.sleep(0.5)
    ser.reset_input_buffer()
    if not wait_shell(ser):
        print("FAIL: no shell")
        return 1
    time.sleep(20.0)
    ser.reset_input_buffer()
    print("push data: %s" % push_file(ser, "_plotdata.txt", DATA), flush=True)
    ser.close()

    sh = boot(port)
    try:
        # ---- Stage A: reference app (axes+grid, sin/cos, world line) ----
        run = run_quiet(sh, "PLOT.BAT", timeout=60)
        check("marker start", "[M-PLOT]" in run, True)
        check("marker done", "[M-PLOT-DONE]" in run, True)
        check("saved", "gfx: saved" in run, True)

        # ---- Stage B: usage/domain errors (transcript only) ----
        check("usage bogus", "unknown subcommand" in run_quiet(sh, "plot bogus"), True)
        out = run_quiet(sh, "plot func")
        check("usage func", "usage: plot func" in out, True)
        # Canvas-dependent checks need an open canvas (PLOT.BAT closed its).
        run_quiet(sh, "gfx init 320 200")
        out = run_quiet(sh, "plot data NOPE.TXT")
        check("missing file", "cannot open" in out, True)
        out = run_quiet(sh, "plot status")
        check("status target", "target=canvas" in out, True)
        check("status window", "window:" in out, True)
        out = run_quiet(sh, "plot table \"X*2\" /from:0 /to:4 /step:2")
        check("table rows", "plot.table: 4 8" in out, True)
        out = run_quiet(sh, "plot polar \"1\" 11 & echo EL=%ERRORLEVEL%")
        check("polar EL", "EL=0" in out, True)
        out = run_quiet(sh, "plot para \"5*cos(T)\" \"5*sin(T)\" 13 & echo EL=%ERRORLEVEL%")
        check("para EL", "EL=0" in out, True)
        run_quiet(sh, "plot window -10 10 -10 10")
        out = run_quiet(sh, "plot auto func \"sin(X)\"")
        check("auto fit", "plot: window x=[-10, 10] y=" in out, True)
        out = run_quiet(sh, "plot func \"sin(X)\" /auto & echo EL=%ERRORLEVEL%")
        check("func auto EL", "EL=0" in out, True)
        out = run_quiet(sh, "plot auto & echo EL=%ERRORLEVEL%")
        check("bare auto EL", "EL=0" in out, True)
        run_quiet(sh, "gfx close")

        # ---- Stage C: data polyline on a fresh canvas ----
        for cmd in ["gfx init 320 200", "plot window -10 10 -10 10",
                    "plot data _plotdata.txt 14", "gfx save _PD.BMP", "gfx close"]:
            run_quiet(sh, cmd)

        # ---- Stage D: inline bar chart ----
        for cmd in ["gfx init 320 200", "plot window 0 4 0 10",
                    "plot bar 2,8,4,6 10", "gfx save _PB.BMP", "gfx close"]:
            run_quiet(sh, cmd)

        # ---- Stage E: point + world line ----
        for cmd in ["gfx init 320 200", "plot window -10 10 -10 10",
                    "plot point 0 0 15", "plot line -10 -10 10 10 15",
                    "gfx save _PP.BMP", "gfx close"]:
            run_quiet(sh, cmd)

        # ---- Stage F: TUI target, verified by screenshot ----
        run_quiet(sh, "plot tui on")
        out = run_quiet(sh, "plot status")
        check("tui target", "target=tui" in out, True)
        run_quiet(sh, "plot window 0 10 0 10")
        run_quiet(sh, "plot axes")
        run_quiet(sh, "plot func \"X\" 15")
        ok = screenshot(sh, os.path.join("spikes", "plot_tui.bmp"))
        check("tui shot", ok, True)
        run_quiet(sh, "plot tui off")
        run_quiet(sh, "draw close")
    finally:
        sh.close()

    # ---- Pull canvases and check pixels ----
    p = os.path.join(TMP, "PLOT.BMP")
    if pull(port, "PLOT.BMP", p):
        b = Bmp(p)
        check("dims", (b.w, b.h), (320, 200))
        # sin zero-crossing passes just left of column 160 (240-sample
        # phase), so assert a white run in the crossing region instead.
        check("sin origin", b.count(156, 96, 164, 102, WHITE) > 0, True)
        check("x axis gray", b.px(10, 99), GRAY)
        check("cos top", b.px(160, 37), CYAN)

    p = os.path.join(TMP, "_PD.BMP")
    if pull(port, "_PD.BMP", p):
        b = Bmp(p)
        check("data origin", b.px(160, 99), YELLOW)
        check("data segment", b.count(198, 72, 203, 77, YELLOW) > 0, True)

    p = os.path.join(TMP, "_PB.BMP")
    if pull(port, "_PB.BMP", p):
        b = Bmp(p)
        check("bar tall", b.px(120, 50), GREEN)
        check("bar short", b.px(40, 170), GREEN)

    p = os.path.join(TMP, "_PP.BMP")
    if pull(port, "_PP.BMP", p):
        b = Bmp(p)
        check("point+line", b.px(160, 99), WHITE)

    print("\nRESULT %s (%d fail)" % ("OK" if not FAILS else "FAIL", len(FAILS)))
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main())
