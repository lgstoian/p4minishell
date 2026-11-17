#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""size_budget.py - fail when static DIRAM creeps back over its budget.

Internal DRAM is the scarce pool on the Tab5 (PSRAM is 32 MB). The Wi-Fi task
and the command worker need large contiguous internal blocks, so every static
byte in DIRAM is contended. v1.4.3 moved the math_disp layout grid and the
modal tables out of ``.bss`` into lazily allocated PSRAM; this gate stops the
next 10 KB table from quietly landing back there.

The measurement is read from the artifacts a normal build already produces:

* ``<build>/*.elf`` - the authoritative path. Sections with ``SHF_ALLOC``
  whose virtual address falls inside an internal-DRAM window are summed. That
  is the same set ``idf.py size`` reports as DIRAM (``.text`` in IRAM plus
  ``.data``/``.bss`` in DRAM); for the default ESP32-P4 windows it reproduces
  ``idf.py size``'s DIRAM byte count exactly.
* ``<build>/*.map`` - fallback for a map without an ELF. Output-section
  headers (column 0) inside the same windows are summed.

Usage:
    python tools/size_budget.py                    # gate on ./build
    python tools/size_budget.py -B build           # explicit build dir
    python tools/size_budget.py --elf firmware.elf # explicit artifact
    python tools/size_budget.py --ceiling 290000   # override the ceiling
    python tools/size_budget.py --json report.json # machine-readable report

Exit codes: 0 within budget, 1 over budget, 2 artifact missing/unreadable
(``--allow-missing`` turns that into a pass so a fresh checkout stays green).

Default ceiling
---------------
``DEFAULT_CEILING_BYTES`` is the static DIRAM measured right after the v1.4.3
modal-tables-to-PSRAM pass (see ``MODAL_TABLES_PASS_DIRAM_BYTES``) plus 4 KiB
of headroom. The headroom is deliberate: small linker/layout churn must not
page an agent, but a table that belongs in PSRAM is 1 KB and up, so the gate
fires long before DIRAM pressure turns into a hardware failure. Raise the
constant only with a measured number in the commit message.
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Static DIRAM measured with this tool immediately after the v1.4.3
# modal-tables move (tui_modal.c: s_frames/s_maps/s_pending/s_cmds/s_scopes/
# s_guards/s_dialogs/s_last_result/s_macros -> lazy PSRAM). Recorded here so
# the default ceiling stays explainable without a git archaeology session.
MODAL_TABLES_PASS_DIRAM_BYTES = 286174

# Headroom granted on top of the measured value (4 KiB, see the module doc).
HEADROOM_BYTES = 4096

DEFAULT_CEILING_BYTES = MODAL_TABLES_PASS_DIRAM_BYTES + HEADROOM_BYTES

# Internal-DRAM windows for the ESP32-P4: sram_low (IRAM/DRAM0) followed by
# sram_high (DRAM1). Both are "internal RAM" for the idf.py size DIRAM figure.
# --dram-window overrides these for other parts or linker layouts.
DEFAULT_DRAM_WINDOWS = (
    (0x4FF00000, 0x80000),
)

SHF_ALLOC = 0x2
SHT_NOBITS = 8

# Linker-script bookkeeping sections occupy DIRAM addresses but hold no
# storage (``.iram0.text_end`` is 118 bytes of alignment padding, and
# ``.dram1.heap_start`` is an 8-byte heap-boundary marker, in the P4 layout).
# idf.py size does not count them; neither do we.
_MARKER_SUFFIXES = ('_end', '.end', '.dummy', '.heap_start', '.heap_end')

_OUTPUT_SECTION_RE = re.compile(
    r'^(\.\S+)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s*$')


def in_windows(addr, size, windows):
    if size <= 0:
        return False
    for origin, length in windows:
        if origin <= addr < origin + length:
            return True
    return False


def is_marker(name):
    """True for linker-script bookkeeping sections (no storage)."""
    return name.endswith(_MARKER_SUFFIXES)


def parse_elf(path, windows):
    """Sum allocatable sections inside the DRAM windows. Returns (used, sections)."""
    with open(path, 'rb') as f:
        data = f.read()
    if len(data) < 0x34 or data[:4] != b'\x7fELF':
        raise ValueError('%s is not an ELF file' % path)
    if data[4] != 1:
        raise ValueError('%s is not a 32-bit ELF (EI_CLASS=%d)'
                         % (path, data[4]))
    if data[5] != 1:
        raise ValueError('%s is not a little-endian ELF (EI_DATA=%d)'
                         % (path, data[5]))
    e_shoff = struct.unpack_from('<I', data, 0x20)[0]
    e_shentsize = struct.unpack_from('<H', data, 0x2E)[0]
    e_shnum = struct.unpack_from('<H', data, 0x30)[0]
    e_shstrndx = struct.unpack_from('<H', data, 0x32)[0]
    if e_shoff == 0 or e_shnum == 0:
        raise ValueError('%s has no section headers' % path)

    def section(index):
        off = e_shoff + index * e_shentsize
        return struct.unpack_from('<IIIIIIIIII', data, off)

    strtab_off = section(e_shstrndx)[4]
    strtab_size = section(e_shstrndx)[5]
    strtab = data[strtab_off:strtab_off + strtab_size]

    def name_at(offset):
        end = strtab.find(b'\0', offset)
        return strtab[offset:end if end >= 0 else None].decode('latin-1')

    used = 0
    sections = []
    for index in range(e_shnum):
        sh_name, sh_type, sh_flags, sh_addr, _off, sh_size, *_rest = section(index)
        if not (sh_flags & SHF_ALLOC) or sh_size == 0:
            continue
        name = name_at(sh_name)
        if name.startswith('.debug') or is_marker(name):
            continue
        if not in_windows(sh_addr, sh_size, windows):
            continue
        used += sh_size
        sections.append({'name': name, 'addr': sh_addr,
                         'size': sh_size, 'nobits': sh_type == SHT_NOBITS})
    return used, sections


def parse_map(path, windows):
    """Sum output-section headers inside the DRAM windows (no ELF available)."""
    used = 0
    sections = []
    with open(path, 'r', encoding='latin-1') as f:
        for line in f:
            match = _OUTPUT_SECTION_RE.match(line.rstrip('\r\n'))
            if not match:
                continue
            name, addr, size = match.group(1), int(match.group(2), 16), int(match.group(3), 16)
            if is_marker(name):
                continue
            if not in_windows(addr, size, windows):
                continue
            used += size
            sections.append({'name': name, 'addr': addr, 'size': size,
                             'nobits': name.startswith('.bss')})
    return used, sections


def find_artifact(build_dir):
    """Prefer the ELF; fall back to the map. Returns (kind, path) or (None, None)."""
    for pattern, kind in (('*.elf', 'elf'), ('*.map', 'map')):
        found = sorted(glob.glob(os.path.join(build_dir, pattern)))
        if found:
            return kind, found[0]
    return None, None


def human(n):
    if n >= 1024 * 1024:
        return '%.2f MB' % (n / (1024.0 * 1024.0))
    if n >= 1024:
        return '%.1f KB' % (n / 1024.0)
    return '%d B' % n


def main(argv=None):
    parser = argparse.ArgumentParser(
        description='Fail when static DIRAM exceeds its budget.')
    parser.add_argument('-B', '--build-dir', default=os.path.join(ROOT, 'build'),
                        help='ESP-IDF build directory (default: ./build)')
    parser.add_argument('--elf', help='explicit ELF or map artifact')
    parser.add_argument('--ceiling', type=int, default=DEFAULT_CEILING_BYTES,
                        help='static DIRAM ceiling in bytes (default: %d = '
                             'measured %d + %d headroom)'
                             % (DEFAULT_CEILING_BYTES, MODAL_TABLES_PASS_DIRAM_BYTES,
                                HEADROOM_BYTES))
    parser.add_argument('--dram-window', action='append', metavar='ORIGIN:SIZE',
                        help='internal-DRAM window as hex ORIGIN:SIZE (repeatable)')
    parser.add_argument('--allow-missing', action='store_true',
                        help='pass when no ELF/map exists (fresh checkout)')
    parser.add_argument('--json', metavar='PATH', help='write a JSON report')
    parser.add_argument('--list', action='store_true',
                        help='list the counted sections')
    parser.add_argument('-q', '--quiet', action='store_true',
                        help='only print the failure line')
    args = parser.parse_args(argv)

    windows = DEFAULT_DRAM_WINDOWS
    if args.dram_window:
        windows = tuple((int(item.split(':')[0], 0), int(item.split(':')[1], 0))
                        for item in args.dram_window)

    if args.elf:
        kind = 'map' if args.elf.endswith('.map') else 'elf'
        artifact = args.elf
    else:
        kind, artifact = find_artifact(args.build_dir)

    if artifact is None or not os.path.exists(artifact):
        message = 'size budget: no ELF or map in %s' % args.build_dir
        if args.allow_missing:
            if not args.quiet:
                print('SKIP %s (fresh checkout)' % message)
            return 0
        print('FAIL %s (build first, or pass --allow-missing)' % message)
        return 2

    try:
        if kind == 'elf':
            used, sections = parse_elf(artifact, windows)
        else:
            used, sections = parse_map(artifact, windows)
    except (OSError, ValueError, struct.error) as exc:
        print('FAIL size budget: cannot read %s: %s' % (artifact, exc))
        return 2

    ceiling = args.ceiling
    headroom = ceiling - used
    over = used > ceiling
    verdict = 'FAIL' if over else 'OK'

    if not args.quiet:
        print('static DIRAM: %d bytes (%s) via %s'
              % (used, human(used), os.path.basename(artifact)))
        print('ceiling:      %d bytes (%s), headroom %d bytes'
              % (ceiling, human(ceiling), headroom))
        if args.list:
            for entry in sorted(sections, key=lambda item: -item['size']):
                print('  %8d  %-24s 0x%08x%s'
                      % (entry['size'], entry['name'], entry['addr'],
                         ' (nobits)' if entry['nobits'] else ''))

    print('%s size budget: static DIRAM %d bytes %s ceiling %d bytes'
          % (verdict, used, '<=' if not over else '>', ceiling))

    if args.json:
        with open(args.json, 'w', encoding='utf-8') as f:
            json.dump({
                'artifact': artifact,
                'kind': kind,
                'used': used,
                'ceiling': ceiling,
                'headroom': headroom,
                'over': over,
                'windows': [{'origin': origin, 'length': length}
                            for origin, length in windows],
                'sections': sections,
            }, f, indent=2)
            f.write('\n')

    return 1 if over else 0


if __name__ == '__main__':
    sys.exit(main())
