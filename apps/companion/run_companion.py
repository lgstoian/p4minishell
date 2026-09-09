#!/usr/bin/env python3
"""run_companion.py - RETIRED driver stub.

The pre-TUI text scenarios this driver asserted ("Enter choice (1-6):",
"Refresh (R) or Back (B)") no longer exist: the companion BATs drive
serial-silent LVGL `list`/`dialog`/`ask` modals, so every scenario timed
out. Use the marker-driven `deep_test.py` (8/8 on hardware) instead.

This stub forwards to it so old invocations keep working:
    python run_companion.py [COMx]  ->  python deep_test.py [COMx]
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import deep_test

print("run_companion: retired, running deep_test instead "
      "(pre-TUI triggers cannot match modal BATs)")
# deep_test takes only an optional COMx port; drop legacy scenario names.
sys.argv = [sys.argv[0]] + [a for a in sys.argv[1:]
                             if a.upper().startswith("COM")]
sys.exit(deep_test.main())
