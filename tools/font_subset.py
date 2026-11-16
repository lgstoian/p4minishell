#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-FileCopyrightText: Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
#
# Adapted from ESP-Brookesia
# gui/brookesia_gui_interface/tools/font/charset_filter.py (Apache-2.0).
# Local changes: the input charset file is now optional (the GB2312/GBK
# candidate sets are generated from the encoding tables, so no bundled charset
# data is needed); P4MiniShell-specific defaults for `--ui-strings`; the
# branding/comments were reworded. See licence.md for the Apache-2.0 terms.
"""font_subset.py - build a small CJK/English charset for font subsetting.

Produces a UTF-8 character set covering ASCII/Latin-1, common CJK punctuation,
fullwidth forms, the selected GB2312 level (or the whole GBK Han range), and
any characters forced by `--ui-strings`/`--extra-symbols-file`. Feed the result
to `fonttools subset` (or `lv_font_conv`) to shrink a TTF/OTF before it goes
into flash.

Typical usage:

  # GB2312 level-1 common Chinese + English/symbols, written to build/.
  python tools/font_subset.py --output charset_gb2312_l1.txt --gb-level l1

  # Level-1 + level-2, keeping the firmware's own UI strings.
  python tools/font_subset.py --output charset_gb2312_l1_l2.txt \
      --gb-level l2 --ui-strings apps/companion/SET.BAT --stats stats.json

  # Intersect with an existing charset file, then subset a font in one step.
  python tools/font_subset.py --input charset_zh_en_gbk.txt \
      --output charset_gb2312_l1.txt --gb-level l1 \
      --font-path /usr/share/fonts/truetype/wqy/wqy-microhei.ttc \
      --subset-output wqy-microhei-gb2312-l1.ttf

Relative output paths are placed under <script_dir>/build/ by default; use
`--build-dir` to override. The filtering step uses only the Python standard
library; the optional TTF/OTF subsetting step needs fonttools.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_BUILD_DIR = SCRIPT_DIR / "build"

DEFAULT_EXTRA_SYMBOLS = (
    "℃°㎡μΩ±×÷←↑→↓↔↕…—–·￥€$£"
    "✓✔✕✖●○◉◎◆◇■□▲▼◀▶★☆"
    "•⌁⌂⌘⌫⏎⏻⏱⏲☀☁☂☃☕⚙⚠"
)

BASIC_SYMBOL_RANGES = (
    (0x0020, 0x007E),  # ASCII: English, digits, basic punctuation
    (0x00A0, 0x00FF),  # Latin-1 supplement
    (0x2000, 0x206F),  # General punctuation
    (0x3000, 0x303F),  # CJK symbols and punctuation
    (0xFF00, 0xFFEF),  # Halfwidth and fullwidth forms
)


def die(message: str, exit_code: int = 1) -> None:
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(exit_code)


def read_chars(path: Path, *, ignore_whitespace: bool = True) -> set[str]:
    if not path.exists():
        die(f"file not found: {path}")
    text = path.read_text(encoding="utf-8")
    if ignore_whitespace:
        return {ch for ch in text if ch not in "\r\n\t"}
    return set(text)


def read_chars_from_many(paths, *, ignore_whitespace: bool = True) -> set[str]:
    chars: set[str] = set()
    for path in paths:
        chars |= read_chars(path, ignore_whitespace=ignore_whitespace)
    return chars


def write_chars(path: Path, chars, *, append_newline: bool = False) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = "".join(sorted(chars, key=ord))
    if append_newline:
        text += "\n"
    path.write_text(text, encoding="utf-8")


def resolve_output_path(path, build_dir: Path):
    if path is None:
        return None
    if path.is_absolute():
        return path
    return build_dir / path


def is_cjk_han(ch: str) -> bool:
    cp = ord(ch)
    return (
        0x3400 <= cp <= 0x4DBF or    # CJK Extension A
        0x4E00 <= cp <= 0x9FFF or    # CJK Unified Ideographs
        0xF900 <= cp <= 0xFAFF or    # CJK Compatibility Ideographs
        0x20000 <= cp <= 0x2A6DF or  # CJK Extension B
        0x2A700 <= cp <= 0x2B73F or  # CJK Extension C
        0x2B740 <= cp <= 0x2B81F or  # CJK Extension D
        0x2B820 <= cp <= 0x2CEAF or  # CJK Extension E/F
        0x2CEB0 <= cp <= 0x2EBEF     # CJK Extension F/G/H range area
    )


def is_basic_symbol(ch: str, extra_symbols: str = DEFAULT_EXTRA_SYMBOLS) -> bool:
    cp = ord(ch)
    for start, end in BASIC_SYMBOL_RANGES:
        if start <= cp <= end:
            return True
    return ch in extra_symbols


def normalize_gb_level(value: str) -> str:
    normalized = value.strip().lower().replace("_", "-")
    aliases = {
        "0": "none", "none": "none", "no": "none", "off": "none",
        "1": "l1", "l1": "l1", "level1": "l1", "level-1": "l1",
        "gb2312-l1": "l1", "gb2312-level1": "l1", "gb2312-level-1": "l1",
        "2": "l2", "l2": "l2", "level2": "l2", "level-2": "l2",
        "gb2312-l2": "l2", "gb2312-l1-l2": "l2", "gb2312-level2": "l2",
        "gb2312-level-2": "l2", "gb2312-all": "l2",
        "gbk": "gbk", "gbk-cjk": "gbk", "gbk-han": "gbk",
    }
    if normalized not in aliases:
        die("unsupported --gb-level. Use one of: none, l1, l2, gbk.")
    return aliases[normalized]


def build_gb2312_chars(level: str) -> set[str]:
    """GB2312 Han rows: level 1 = rows 16-55, level 2 adds rows 56-87."""
    if level == "l1":
        row_start, row_end_inclusive = 16, 55
    elif level == "l2":
        row_start, row_end_inclusive = 16, 87
    else:
        die(f"internal error: unsupported GB2312 level: {level}")

    chars: set[str] = set()
    for row in range(row_start, row_end_inclusive + 1):
        for cell in range(1, 95):
            high = row + 0xA0
            low = cell + 0xA0
            try:
                ch = bytes([high, low]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            if len(ch) == 1 and is_cjk_han(ch):
                chars.add(ch)
    return chars


def build_gbk_cjk_chars() -> set[str]:
    """A broad Han set decoded from the GBK double-byte space."""
    chars: set[str] = set()
    for high in range(0x81, 0xFF):
        for low in list(range(0x40, 0x7F)) + list(range(0x80, 0xFF)):
            try:
                ch = bytes([high, low]).decode("gbk")
            except UnicodeDecodeError:
                continue
            if len(ch) == 1 and is_cjk_han(ch):
                chars.add(ch)
    return chars


def build_gb_chars(gb_level: str) -> set[str]:
    gb_level = normalize_gb_level(gb_level)
    if gb_level == "none":
        return set()
    if gb_level in {"l1", "l2"}:
        return build_gb2312_chars(gb_level)
    if gb_level == "gbk":
        return build_gbk_cjk_chars()
    die(f"internal error: unknown normalized GB level: {gb_level}")


def run_fonttools_subset(*, font_path: Path, charset_path: Path,
                         output_path: Path, font_number: int,
                         fonttools_bin: str, no_hinting: bool) -> None:
    if not font_path.exists():
        die(f"font file not found: {font_path}")
    if shutil.which(fonttools_bin) is None:
        die(f"fonttools executable not found: {fonttools_bin}. "
            "Install it with: pip install fonttools")
    cmd = [
        fonttools_bin, "subset", str(font_path),
        f"--font-number={font_number}",
        f"--text-file={charset_path}",
        f"--output-file={output_path}",
        "--layout-features=*", "--glyph-names", "--symbol-cmap",
        "--legacy-cmap", "--notdef-glyph", "--notdef-outline",
        "--recommended-glyphs", "--name-IDs=*", "--name-legacy",
        "--name-languages=*", "--ignore-missing-glyphs",
    ]
    if no_hinting:
        cmd.append("--no-hinting")
    print("running:", " ".join(cmd))
    subprocess.run(cmd, check=True)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build a small CJK/English charset for font subsetting.",
        formatter_class=argparse.RawTextHelpFormatter,
    )
    parser.add_argument("-i", "--input", type=Path, default=None,
                        help="Optional source charset file; the result is "
                             "intersected with it. Omit to use the generated "
                             "GB level + symbol ranges alone.")
    parser.add_argument("-o", "--output", required=True, type=Path,
                        help="Output charset file.")
    parser.add_argument("--removed", type=Path, default=None,
                        help="Optional output for characters not kept.")
    parser.add_argument("--stats", type=Path, default=None,
                        help="Optional JSON stats output file.")
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD_DIR,
                        help="Directory for relative outputs "
                             "(default: <script_dir>/build).")
    parser.add_argument("--gb-level", default="l1",
                        help="Chinese charset level: none, l1 (GB2312 level-1), "
                             "l2 (level-1+2), gbk. Default: l1.")
    parser.add_argument("--no-basic-symbols", action="store_true",
                        help="Do not keep ASCII/Latin-1/CJK-punctuation/"
                             "fullwidth symbols automatically.")
    parser.add_argument("--extra-symbols", default=DEFAULT_EXTRA_SYMBOLS,
                        help="Extra symbols to keep.")
    parser.add_argument("--extra-symbols-file", action="append", type=Path,
                        default=[])
    parser.add_argument("--ui-strings", "--force-keep-file",
                        dest="force_keep_files", action="append", type=Path,
                        default=[])
    parser.add_argument("--exclude-file", action="append", type=Path, default=[])
    parser.add_argument("--keep-whitespace", action="store_true",
                        help="Keep CR/LF/TAB from inputs (default drops them).")
    parser.add_argument("--append-newline", action="store_true",
                        help="Append a newline to output files.")
    parser.add_argument("--font-path", type=Path, default=None,
                        help="Optional font file to subset after filtering.")
    parser.add_argument("--font-number", type=int, default=0,
                        help="Font index for TTC files. Default: 0.")
    parser.add_argument("--subset-output", type=Path, default=None,
                        help="Optional subset font output path.")
    parser.add_argument("--fonttools-bin", default="fonttools")
    parser.add_argument("--no-hinting", action="store_true",
                        help="Pass --no-hinting to fonttools subset.")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    gb_level = normalize_gb_level(args.gb_level)
    ignore_whitespace = not args.keep_whitespace

    build_dir = args.build_dir
    build_dir.mkdir(parents=True, exist_ok=True)

    output_path = resolve_output_path(args.output, build_dir)
    removed_path = resolve_output_path(args.removed, build_dir)
    stats_path = resolve_output_path(args.stats, build_dir)
    subset_output_path = resolve_output_path(args.subset_output, build_dir)

    source_chars = read_chars(args.input, ignore_whitespace=ignore_whitespace) \
        if args.input else set()

    keep_chars: set[str] = set()
    if not args.no_basic_symbols:
        keep_chars |= {ch for ch in source_chars
                       if is_basic_symbol(ch, args.extra_symbols)}
        keep_chars |= {ch for ch in args.extra_symbols
                       if not ignore_whitespace or ch not in "\r\n\t"}
        # Without an input file there is no source to intersect: keep the whole
        # basic-symbol range so English/punctuation survive.
        if not args.input:
            for start, end in BASIC_SYMBOL_RANGES:
                keep_chars |= {chr(cp) for cp in range(start, end + 1)}

    gb_chars = build_gb_chars(gb_level)
    if args.input:
        keep_chars |= source_chars & gb_chars
    else:
        keep_chars |= gb_chars

    extra_file_chars = read_chars_from_many(
        args.extra_symbols_file, ignore_whitespace=ignore_whitespace)
    force_keep_chars = read_chars_from_many(
        args.force_keep_files, ignore_whitespace=ignore_whitespace)
    exclude_chars = read_chars_from_many(
        args.exclude_file, ignore_whitespace=ignore_whitespace)

    keep_chars |= extra_file_chars
    keep_chars |= force_keep_chars
    keep_chars -= exclude_chars

    removed_chars = source_chars - keep_chars
    added_outside_source = keep_chars - source_chars

    write_chars(output_path, keep_chars, append_newline=args.append_newline)
    if removed_path:
        write_chars(removed_path, removed_chars, append_newline=args.append_newline)

    stats = {
        "input": str(args.input) if args.input else None,
        "output": str(output_path),
        "build_dir": str(build_dir),
        "gb_level": gb_level,
        "source_count": len(source_chars),
        "gb_candidate_count": len(gb_chars),
        "kept_count": len(keep_chars),
        "removed_count": len(removed_chars),
        "added_outside_source_count": len(added_outside_source),
        "forced_char_count": len(force_keep_chars),
        "basic_symbols_enabled": not args.no_basic_symbols,
    }
    if stats_path:
        stats_path.parent.mkdir(parents=True, exist_ok=True)
        stats_path.write_text(json.dumps(stats, ensure_ascii=False, indent=2) + "\n",
                              encoding="utf-8")

    print("charset filtering done")
    print(f"  input chars          : {stats['source_count']}")
    print(f"  GB level             : {gb_level}")
    print(f"  kept chars           : {stats['kept_count']}")
    print(f"  removed chars        : {stats['removed_count']}")
    print(f"  added outside source : {stats['added_outside_source_count']}")
    print(f"  output               : {output_path}")

    if args.font_path or subset_output_path:
        if not args.font_path or not subset_output_path:
            die("--font-path and --subset-output must be used together")
        run_fonttools_subset(
            font_path=args.font_path, charset_path=output_path,
            output_path=subset_output_path, font_number=args.font_number,
            fonttools_bin=args.fonttools_bin, no_hinting=args.no_hinting)
        print(f"  subset font output   : {subset_output_path}")


if __name__ == "__main__":
    main()
