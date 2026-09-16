#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""svc_test.py - background service + alarm scheduler on hardware.

Usage: python svc_test.py [COMx]
1. `start SVC` -> [M-SVC] service started, a tick arrives.
2. `ps` shows bg0 running; `alarm status` reads the scheduler.
3. `taskkill bg0` -> [M-SVC] service stopped.
4. `call AGENDA.BAT` -> [M-AGENDA-DONE].
"""
import sys
import time

sys.path.insert(0, "tools")
from bg_run import run_quiet, boot


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    # Start/stop are async; capture raw with a marker-based drain.
    sh = boot(port)
    ok = True

    def show(cmd, timeout=40):
        out = run_quiet(sh, cmd, timeout=timeout)
        print("=== %s ===" % cmd, flush=True)
        print(out[-500:], flush=True)
        return out

    out = show("start SVC")
    ok = ("[M-SVC] service started" in out) and ok
    time.sleep(3)
    show("ps")
    show("alarm status")
    out = show("taskkill bg0")
    ok = ("stop requested" in out or "[M-SVC] service stopped" in out) and ok
    time.sleep(3)
    show("echo after-stop")
    out = show("call AGENDA.BAT")
    ok = ("M-AGENDA-DONE" in out) and ok
    print("RESULT %s" % ("OK" if ok else "FAIL"), flush=True)
    sh.close()


main()
