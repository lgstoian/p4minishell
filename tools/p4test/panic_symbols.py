# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Symbolize panic backtraces with the firmware ELF that produced them.

A panic tail names raw addresses; without symbols every triage starts with a
manual addr2line call and the board's build directory. The suite runner knows
the port, so it can pick the matching ELF and attach symbols to the failure
text automatically. If the toolchain or ELF is missing this returns "" and the
raw tail is used unchanged - it never turns a panic into a different failure.
"""
import glob
import os
import re
import shutil
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# COM3 is the reference board (build/), COM6 the Tab5 (build-tab5/). Other
# ports fall back to whatever ELF exists so a new board still gets symbols
# when it shares one of the build directories.
BOARD_ELVES = {
    "COM3": os.path.join(ROOT, "build", "p4minishell.elf"),
    "COM6": os.path.join(ROOT, "build-tab5", "p4minishell.elf"),
}

_ADDR_RE = re.compile(r"0x[0-9a-fA-F]{8}")


def _addr2line():
    tool = shutil.which("riscv32-esp-elf-addr2line")
    if tool:
        return tool
    home = os.path.expanduser("~")
    pattern = os.path.join(home, ".espressif", "tools", "riscv32-esp-elf",
                           "*", "riscv32-esp-elf", "bin",
                           "riscv32-esp-elf-addr2line.exe")
    matches = sorted(glob.glob(pattern))
    return matches[-1] if matches else None


def elf_for_port(port):
    """The ELF that matches @p port, or None when it has not been built."""
    if port is None:
        return None
    port = str(port).upper()
    candidates = []
    if port in BOARD_ELVES:
        candidates.append(BOARD_ELVES[port])
    candidates.extend(BOARD_ELVES.values())
    for path in candidates:
        if os.path.exists(path):
            return path
    return None


def symbolize(text, port=None, limit=12):
    """Return an indented symbol block for the addresses in @p text, or ""."""
    if not text:
        return ""
    addrs = []
    for match in _ADDR_RE.findall(text):
        if match not in addrs:
            addrs.append(match)
    if not addrs:
        return ""
    elf = elf_for_port(port)
    tool = _addr2line()
    if elf is None or tool is None:
        return ""
    try:
        result = subprocess.run([tool, "-pfiaC", "-e", elf] + addrs[:limit],
                                capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return ""
    lines = [line.strip() for line in result.stdout.splitlines() if line.strip()]
    if not lines:
        return ""
    return ("  symbols (%s):\n    " % os.path.basename(elf) +
            "\n    ".join(lines))
