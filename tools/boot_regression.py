#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""boot_regression.py - fresh-boot regression guard for the boot fixes.

Resets the board and captures N fresh boots, asserting each one:
  * no panic / assertion / heap-poisoning abort,
  * exactly one AUTOEXEC.BAT run (the deferred boot-script path fires once),
  * the SD card mounted ("SD card ready"),
  * no ESP_LOG W/E lines other than the ROM boot header.

This guards the v0.36.1/v0.37.0 fixes: the boot script is no longer skipped
and no longer runs twice, the header async double-free crash stays gone, and
the expected transient boot logs stay suppressed. Widget/hardware behaviour is
covered by tools/header_test.py and the companion suites.

Usage: python boot_regression.py [COMx] [boots]
"""

import os
import re
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from shell_session import open_port, default_port, hard_reset

PORT = sys.argv[1] if len(sys.argv) > 1 else default_port()
BOOTS = int(sys.argv[2]) if len(sys.argv) > 2 else 10
CAPTURE_S = 18.0

ANSI = re.compile(r"\x1b\[[0-9;]*m")
# ROM/bootloader banner and the memprobe diagnostic are not firmware warnings.
BENIGN_WE = re.compile(r"memprobe|rst:0x|boot: |boot\.esp32p4|esp_image|qio_mode")


def capture_boot(ser):
    ser.timeout = 0.3
    buf = b""
    end = time.time() + CAPTURE_S
    while time.time() < end:
        chunk = ser.read(65536)
        if chunk:
            buf += chunk
    return ANSI.sub("", buf.decode("utf-8", errors="replace"))


def main():
    failures = 0
    for i in range(1, BOOTS + 1):
        # open_port() does not reboot, so reset explicitly to capture each
        # boot from the ROM banner.
        hard_reset(PORT)
        ser = open_port(PORT)
        text = capture_boot(ser)
        ser.close()

        panics = len(re.findall(r"Guru Meditation|panic'ed|Load access fault|"
                                r"Illegal instruction|assert failed|multi_heap", text))
        autoexec = len(re.findall(r"=== AUTOEXEC START ===", text))
        sd_ready = "SD card ready" in text
        warnings = [ln.strip() for ln in text.splitlines()
                    if re.search(r"^[WE] \(\d+\)", ln.strip()) and not BENIGN_WE.search(ln)]

        ok = (panics == 0) and (autoexec == 1) and sd_ready and not warnings
        if not ok:
            failures += 1
        print("boot %2d : %s  panics=%d autoexec=%d sd_ready=%s warn/err=%d"
              % (i, "PASS" if ok else "FAIL", panics, autoexec, sd_ready, len(warnings)),
              flush=True)
        for w in warnings[:5]:
            print("           " + w[:140], flush=True)

    print("\nRESULT %s (%d/%d clean)" %
          ("OK" if failures == 0 else "FAIL", BOOTS - failures, BOOTS))
    return 1 if failures else 0


sys.exit(main())
