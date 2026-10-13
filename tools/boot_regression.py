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

Usage: python boot_regression.py [COMx] [boots] [COMy]
  One or two ports; two ports capture concurrently.
"""

import os
import re
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from shell_session import open_port, default_port, hard_reset

PORTS = [a.upper() for a in sys.argv[1:] if a.upper().startswith("COM")]
BOOTS = [int(a) for a in sys.argv[1:] if re.fullmatch(r"\d+", a)]
PORT = PORTS[0] if PORTS else default_port()
BOOTS = BOOTS[0] if BOOTS else 10
EXTRA_PORTS = PORTS[1:2]
CAPTURE_S = 18.0

ANSI = re.compile(r"\x1b\[[0-9;]*m")
# Lines that are not firmware warnings:
#   - ROM/bootloader banner and the memprobe diagnostic;
#   - the IDF on-chip-LDO "voltage 0" note: the Tab5 BSP used to trigger it via
#     `sd_pwr_ctrl_new_on_chip_ldo` and now owns LDO_VO4 itself, so it should not
#     appear; kept as a defensive guard in case a future BSP regresses;
#   - the Tab5's first hosted RPC, which times out at the SDIO layer before the
#     networking transport-reset retry recovers it (a persistent failure still
#     surfaces as a later Wi-Fi init error, which is not benign).
BENIGN_WE = re.compile(
    r"memprobe|rst:0x|boot: |boot\.esp32p4|esp_image|qio_mode"
    r"|ldo: The voltage value 0 is out of the recommended range"
    r"|sdmmc_io:.*returned 0x107"
    r"|eh_sdio: SDIO aggr: unrecoverable write failure"
    r"|eh_host_feat_rpc: request: no response uid=1 msg_id=350")


def capture_boot(ser):
    ser.timeout = 0.3
    buf = b""
    end = time.time() + CAPTURE_S
    while time.time() < end:
        chunk = ser.read(65536)
        if chunk:
            buf += chunk
    return ANSI.sub("", buf.decode("utf-8", errors="replace"))


def run_one_port(port, boots, out):
    failures = 0
    totals = {}
    lines = []
    for i in range(1, boots + 1):
        # open_port() does not reboot, so reset explicitly to capture each
        # boot from the ROM banner.
        hard_reset(port)
        ser = open_port(port)
        text = capture_boot(ser)
        ser.close()

        panics = len(re.findall(r"Guru Meditation|panic'ed|Load access fault|"
                                r"Illegal instruction|assert failed|multi_heap", text))
        autoexec = len(re.findall(r"Boot Script: running AUTOEXEC\.BAT", text))
        sd_ready = "SD card ready" in text
        warnings = [ln.strip() for ln in text.splitlines()
                    if re.search(r"^[WE] \(\d+\)", ln.strip()) and not BENIGN_WE.search(ln)]

        # bugs.md F6 signatures: the tolerated boot noise is counted even when
        # benign, so a fix can be graded on storm volume, not just PASS/FAIL.
        sigs = {}
        for name, pat in (
                ("txbuf_err", r"sdio_get_tx_buffer_num: err"),
                ("sdmmc_107", r"sdmmc_io:.*returned 0x107"),
                ("rpc350", r"no response uid=1 msg_id=350"),
                ("aggr_fail", r"SDIO aggr: unrecoverable write failure"),
                ("task_wdt", r"Task watchdog got triggered"),
                ("backtrace", r"Backtrace:")):
            n = len(re.findall(pat, text))
            if n:
                sigs[name] = n
                totals[name] = totals.get(name, 0) + n

        ok = (panics == 0) and (autoexec == 1) and sd_ready and not warnings
        if not ok:
            failures += 1
        lines.append("boot %2d : %s  panics=%d autoexec=%d sd_ready=%s warn/err=%d%s"
                     % (i, "PASS" if ok else "FAIL", panics, autoexec, sd_ready, len(warnings),
                        ("  f6[" + " ".join("%s=%d" % kv for kv in sorted(sigs.items())) + "]")
                        if sigs else ""))
        for w in warnings[:5]:
            lines.append("           " + w[:140])
    lines.append("")
    lines.append("RESULT %s (%d/%d clean)  f6_totals=%s" %
                 ("OK" if failures == 0 else "FAIL", boots - failures, boots,
                    totals if totals else "clean"))
    out.append((port, failures == 0, lines))


def main():
    ports = [PORT] + EXTRA_PORTS
    multi = len(ports) > 1
    out = []
    threads = [threading.Thread(target=run_one_port, args=(p, BOOTS, out),
                                name="boot-%s" % p, daemon=True)
               for p in ports]
    for th in threads:
        th.start()
    for th in threads:
        th.join()
    failed = 0
    for port, ok, lines in sorted(out):
        for line in lines:
            if multi and line:
                print("[%s] %s" % (port, line), flush=True)
            else:
                print(line, flush=True)
        if not ok:
            failed += 1
    if multi:
        print("BOOT MERGED %s"
              % " ".join("%s:%s" % (p, "OK" if ok else "FAIL")
                         for p, ok, _l in sorted(out)), flush=True)
    return 1 if failed else 0


sys.exit(main())
