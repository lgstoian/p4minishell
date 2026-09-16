#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""bounce_test.py - run BOUNCE.BAT to completion + mid-flight screenshot.

Usage: python bounce_test.py [COMx]
"""
import struct
import sys
import time

sys.path.insert(0, "tools")
from bg_run import boot, run_quiet, marker_done


def shot(sh, out):
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
                print("saved %s" % out, flush=True)
                return
    print("screenshot TIMEOUT", flush=True)


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    sh = boot(port)
    # SD mount misses some boots (transient): retry before relying on it.
    # (Match the dir SIZE line, not the command echo.)
    out = run_quiet(sh, "dir BOUNCE.BAT")
    if "KiB  /sdcard/BOUNCE.BAT" not in out and " B  /sdcard/BOUNCE.BAT" not in out:
        print("SD missed this boot, remounting...", flush=True)
        run_quiet(sh, "sd mount")
        out = run_quiet(sh, "dir BOUNCE.BAT")
        if "BOUNCE.BAT" not in out.replace("dir BOUNCE.BAT", "", 1):
            print("SD still missing, aborting", flush=True)
            sh.close()
            return
    # Pass 1: run to completion, count frame markers.
    sh.s.reset_input_buffer()
    sh.s.write(b"BOUNCE\r\n")
    sh.s.timeout = 0.5
    end = time.time() + 90
    data = b""
    done = False
    while time.time() < end:
        chunk = sh.s.read(65536)
        if chunk:
            data += chunk
            # Substring is safe: "@echo off" never echoes the batch's own
            # echo lines, so markers appear only as worker output.
            if b"M-BOUNCE-DONE" in data:
                done = True
                break
    text = data.decode("utf-8", errors="replace")
    print("done=%s frames=%d" % (done, text.count("M-BOUNCE-FRAME")), flush=True)
    i = text.find("M-BOUNCE-DONE")
    if i >= 0:
        print(text[max(0, i - 100):i + 40][-160:], flush=True)
    if not done:
        print("BOUNCE did not finish, aborting", flush=True)
        sh.close()
        return
    # Pass 2: relaunch and screenshot mid-flight (canvas visible ~10 s).
    sh.s.reset_input_buffer()
    sh.s.write(b"BOUNCE\r\n")
    time.sleep(5)
    shot(sh, "screenshots/bounce_mid.bmp")
    sh.close()


main()
