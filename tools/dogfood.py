#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""dogfood.py - autonomous remote-over-USB dogfooding of the firmware.

Drives the board the way a curious user would (help, status, files, apps,
modals, TUI and canvas animation), captures a screenshot after every action,
and journals anomalies (panics, timeouts, blue "BSOD" frames, blank frames,
heap decline). Reproducible from --seed.

Usage:
    python tools/dogfood.py COM3 --minutes 15
    python tools/dogfood.py COM3 --minutes 3 --seed 7 --out screenshots/dogfood/run7
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from p4test.agent import DogfoodAgent  # noqa: E402
from p4test.device import Device  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description="P4MiniShell autonomous dogfooding")
    ap.add_argument("port", nargs="?", default=None, help="COMx (argv/P4_PORT/COM11)")
    ap.add_argument("--minutes", type=float, default=10.0)
    ap.add_argument("--seed", type=int, default=int(time.time()))
    ap.add_argument("--out", default=None)
    ap.add_argument("--quick", action="store_true")
    args = ap.parse_args()

    out = args.out or os.path.join("screenshots", "dogfood",
                                   time.strftime("run_%Y%m%d_%H%M%S"))
    os.makedirs(out, exist_ok=True)
    print("dogfood: seed=%d minutes=%.1f out=%s" % (args.seed, args.minutes, out))

    dev = Device(args.port, boot=True, settle_ms=4000 if args.quick else 8000)
    try:
        agent = DogfoodAgent(dev, out, seed=args.seed, quick=args.quick)
        c = agent.run(minutes=args.minutes)
        c.print_report()
        return 0 if c.ok else 1
    finally:
        dev.close()


if __name__ == "__main__":
    sys.exit(main())
