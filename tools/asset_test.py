#!/usr/bin/env python3
"""asset_test.py - HW verify crc32/asset verbs.

Usage: python asset_test.py [COMx]
1. crc32 SPR32.BMP vs host zlib (must match exactly).
2. Build TESTA.ASSETS manifest on host (real CRCs), push to APPS/,
   asset list + asset check -> OK.
3. Tamper: manifest with wrong CRC -> MISMATCH; missing file -> MISSING;
   bad app name -> usage.
"""
import os
import sys
import time
import zlib

sys.path.insert(0, "tools")
sys.path.insert(0, os.path.join("apps", "companion"))
from push_sd import push_file, wait_shell  # noqa: E402
from bg_run import run_quiet, boot  # noqa: E402
from shell_session import open_port  # noqa: E402


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    with open(os.path.join("screenshots", "spr32.bmp"), "rb") as f:
        spr = f.read()
    print("host crc SPR32.BMP: %08X" % (zlib.crc32(spr) & 0xFFFFFFFF), flush=True)
    manifest = ("# test manifest\n; version=1\nSPR32.BMP=%08X\n"
                % (zlib.crc32(spr) & 0xFFFFFFFF)).encode()
    bad = b"SPR32.BMP=00000000\nNOPE.BAT=12345678\n"
    ser = open_port(port, 115200, 1)
    ser.reset_input_buffer()
    if not wait_shell(ser):
        print("FAIL: no shell", flush=True)
        return
    time.sleep(20.0)
    ser.reset_input_buffer()
    print("push manifest: %s" % push_file(ser, "APPS/TESTA.ASSETS", manifest), flush=True)
    ser.close()

    sh = boot(port)
    for cmd in ["crc32 SPR32.BMP", "asset list TESTA", "asset check TESTA",
                "echo EL=%ERRORLEVEL%"]:
        try:
            out = run_quiet(sh, cmd)
        except RuntimeError as e:
            print("PANIC after %r: %s" % (cmd, e), flush=True)
            break
        print("=== %s ===" % cmd, flush=True)
        print(out[-350:], flush=True)

    # Overwrite manifest with bad entries, re-check.
    sh.close()
    ser = open_port(port, 115200, 1)
    ser.reset_input_buffer()
    wait_shell(ser)
    time.sleep(8.0)
    ser.reset_input_buffer()
    print("push bad manifest: %s" % push_file(ser, "APPS/TESTA.ASSETS", bad), flush=True)
    ser.close()
    sh2 = boot(port)
    for cmd in ["asset check TESTA", "echo EL=%ERRORLEVEL%",
                "asset check NO-SUCH!!", "asset check NOAPP"]:
        try:
            out = run_quiet(sh2, cmd)
        except RuntimeError as e:
            print("PANIC after %r: %s" % (cmd, e), flush=True)
            break
        print("=== %s ===" % cmd, flush=True)
        print(out[-350:], flush=True)
    sh2.close()


main()
