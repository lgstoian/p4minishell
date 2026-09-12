#!/usr/bin/env python3
"""asset_test2.py - negative paths (bad CRC, missing file, bad app).

Usage: python asset_test2.py [COMx]
Assumes APPS/TESTA.ASSETS currently holds the GOOD manifest; overwrites
with bad entries, then checks.
"""
import os
import sys
import time

sys.path.insert(0, "tools")
sys.path.insert(0, os.path.join("apps", "companion"))
from push_sd import push_file, wait_shell  # noqa: E402
from bg_run import run_quiet, boot  # noqa: E402
from shell_session import open_port  # noqa: E402


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    bad = b"SPR32.BMP=00000000\nNOPE.BAT=12345678\n"
    ser = open_port(port, 115200, 1)
    ser.reset_input_buffer()
    if not wait_shell(ser):
        print("FAIL: no shell", flush=True)
        return
    time.sleep(8.0)
    ser.reset_input_buffer()
    print("push bad manifest: %s" % push_file(ser, "APPS/TESTA.ASSETS", bad), flush=True)
    ser.close()
    sh = boot(port)
    for cmd in ["asset check TESTA", "echo EL=%ERRORLEVEL%",
                "asset check NO-SUCH!!", "asset check NOAPP",
                "crc32 NOPE.BAT"]:
        try:
            out = run_quiet(sh, cmd)
        except RuntimeError as e:
            print("PANIC after %r: %s" % (cmd, e), flush=True)
            break
        print("=== %s ===" % cmd, flush=True)
        print(out[-350:], flush=True)
    sh.close()


main()
