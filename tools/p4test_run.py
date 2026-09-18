#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""p4test_run.py - run the P4MiniShell hardware regression suites.

Usage:
    python tools/p4test_run.py COM3
    python tools/p4test_run.py COM3 --list
    python tools/p4test_run.py COM3 --only s01_smoke,s05_storage
    python tools/p4test_run.py COM3 --tag core
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from p4test import runner  # noqa: E402

if __name__ == "__main__":
    sys.exit(runner.main())
