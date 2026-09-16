# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
import sys

sys.path.insert(0, "tools")
from shell_session import Shell

sh = Shell()
for cmd in ["tui status", "version", "mem", "wifi status"]:
    print("=" * 20, cmd, "=" * 20)
    print(sh.run(cmd, timeout=30)[:1500])
sh.close()
