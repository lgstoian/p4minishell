# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""BSOD watcher: timestamped serial capture for 25 minutes."""
import time
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from shell_session import open_port, default_port

s = open_port(default_port(), 115200, timeout=1)
t0 = time.time()
with open("bsod_watch.log", "wb") as f:
    while time.time() - t0 < 25 * 60:
        d = s.read(4096)
        if d:
            f.write(("[%.1f] " % (time.time() - t0)).encode() + d + b"\n")
            f.flush()
s.close()
print("watch done")
