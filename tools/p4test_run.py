#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""p4test_run.py - run the P4MiniShell hardware regression suites.

Usage:
    python tools/p4test_run.py COM3
    python tools/p4test_run.py COM3 --list
    python tools/p4test_run.py COM3 --only s01_smoke,s07_data
    python tools/p4test_run.py COM3 --tag core
    python tools/p4test_run.py --boards COM3,COM6 --tier fast --fail-fast
    python tools/p4test_run.py --boards m5stack_tab5 --only s12_power_audio

Tiers: --tier fast runs the per-area smoke subset (feature delivery);
--tier full (default) runs every suite (releases). --boards runs the
selection concurrently on several boards with one merged table.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from p4test import runner  # noqa: E402

if __name__ == "__main__":
    sys.exit(runner.main())
