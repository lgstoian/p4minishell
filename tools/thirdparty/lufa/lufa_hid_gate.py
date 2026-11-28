#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""lufa_hid_gate.py - host-only LUFA-backed validation of shipped USB HID code.

LUFA is an AVR8 USB stack and cannot run on the ESP32-P4. This gate uses only
(a) a verbatim subset of its named HID constants and (b) the IDEA of its
canonical boot descriptors, re-expressed as a small pure helper in our style
(lufa_hid_reference.c). The host checker compiles that helper TOGETHER WITH
the shipped decoder components/usb/usb_hid_layout.c, so the generated mouse
descriptor is really decoded by production code; no second decoder or USB
stack is introduced.

Usage:
    python tools/thirdparty/lufa/lufa_hid_gate.py --check
    python tools/thirdparty/lufa/lufa_hid_gate.py --self-test   # red canaries
"""
from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import List, Tuple

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
LICENCE = HERE / "LICENSE.txt"
CHECKER = HERE / "lufa_hid_fixture_check.c"
REFERENCE = HERE / "lufa_hid_reference.c"
CONSTANTS = HERE / "lufa_hid_constants.h"
SHIPPED_DECODER = ROOT / "components" / "usb" / "usb_hid_layout.c"
SHIPPED_HEADER = ROOT / "components" / "usb" / "usb_hid_layout.h"
USB_C = ROOT / "components" / "usb" / "usb.c"
LAYOUT_TEST = ROOT / "test" / "main" / "test_usb_hid_layout.c"

# Verified from upstream itself: /repos/abcminiuser/lufa/contents/LUFA/License.txt
# at commit 90d65ba059d91078a34b9c26c8772ee14b556a13, blob sha f63a25f8...
UPSTREAM_LICENCE_BLOB_SHA1 = "f63a25f81e33e40029dadc349fc82d6eeea8b28b"
PINNED_COMMIT = "90d65ba059d91078a34b9c26c8772ee14b556a13"
CC_FLAGS = ["-std=c11", "-Wall", "-Wextra", "-Werror",
            "-I", str(HERE), "-I", str(SHIPPED_DECODER.parent)]


def git_blob_sha1(data: bytes) -> str:
    """The SHA-1 git stores for a blob: sha1(b'blob <len>\\0' + data)."""
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


def static_guards() -> Tuple[int, List[str]]:
    """Licence verbatim; host-only sources present; no LUFA in firmware."""
    lines: List[str] = []
    failed = 0

    if not LICENCE.is_file():
        lines.append("FAIL missing vendored LICENSE.txt")
        return 1, lines
    # Compare canonical LF content: a checkout on Windows may materialise the
    # verbatim upstream file with CRLF, which changes the raw blob SHA without
    # changing a single licence byte.
    data = LICENCE.read_bytes().replace(b"\r\n", b"\n")
    blob = git_blob_sha1(data)
    if blob != UPSTREAM_LICENCE_BLOB_SHA1:
        lines.append("FAIL LICENSE.txt blob sha %s != upstream %s"
                     % (blob, UPSTREAM_LICENCE_BLOB_SHA1))
        failed += 1
    else:
        lines.append("OK   LICENSE.txt verbatim (blob %s, sha256 %s)"
                     % (blob[:12], hashlib.sha256(data).hexdigest()[:16]))

    for path in (CONSTANTS, REFERENCE, HERE / "lufa_hid_reference.h", CHECKER,
                 SHIPPED_DECODER, SHIPPED_HEADER):
        if not path.is_file():
            lines.append("FAIL missing required source %s" % path.name)
            failed += 1
    readme = HERE / "README.md"
    if not readme.is_file() or any(
            token not in readme.read_text(encoding="utf-8")
            for token in (PINNED_COMMIT, "Dean Camera", "LICENSE.txt")):
        lines.append("FAIL README.md does not document provenance")
        failed += 1

    # No second USB stack: no LUFA reference in firmware or its build files.
    offenders: List[str] = []
    text_suffixes = {".c", ".h", ".cpp", ".hpp", ".cmake", ".txt", ".yml", ".kconfig"}
    for area in ("components", "main", "coprocessor", "managed_components", "test"):
        base = ROOT / area
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            if not path.is_file():
                continue
            if (path.suffix.lower() not in text_suffixes
                    and not path.name.startswith("sdkconfig")):
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="ignore")
            except OSError:
                continue
            if "lufa" in text.lower():
                offenders.append(str(path.relative_to(ROOT)).replace("\\", "/"))
    for path in (ROOT / "CMakeLists.txt",):
        if path.is_file() and "lufa" in path.read_text(
                encoding="utf-8", errors="ignore").lower():
            offenders.append(path.name)
    for path in ROOT.glob("sdkconfig*"):
        if path.is_file() and "lufa" in path.read_text(
                encoding="utf-8", errors="ignore").lower():
            offenders.append(path.name)
    if offenders:
        lines.append("FAIL LUFA referenced in firmware/build: %s"
                     % ", ".join(offenders[:8]))
        failed += 1
    else:
        lines.append("OK   no LUFA reference in firmware or ESP-IDF build files")

    return failed, lines


def compile_checker(build: Path) -> Tuple[int, str]:
    cc = os.environ.get("CC") or shutil.which("gcc") or shutil.which("cc")
    if not cc:
        return 1, "gcc/cc not found: install a C compiler to run this gate"
    exe = build / ("lufa_hid_check.exe" if os.name == "nt" else "lufa_hid_check")
    cmd = [cc, *CC_FLAGS, str(CHECKER), str(REFERENCE), str(SHIPPED_DECODER),
           "-o", str(exe)]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    output = (proc.stdout or "") + (proc.stderr or "")
    if proc.returncode != 0:
        return proc.returncode, "compile failed: %s\n%s" % (" ".join(cmd), output)
    return 0, str(exe)


def run_checker(exe: str, usb: Path, layout: Path) -> subprocess.CompletedProcess:
    return subprocess.run([exe, str(usb), str(layout)], capture_output=True,
                          text=True, timeout=60)


def do_check() -> int:
    for area in USB_C, LAYOUT_TEST:
        if not area.is_file():
            print("  FAIL missing shipped input %s" % area, file=sys.stderr)
            return 2
    failed, lines = static_guards()
    for line in lines:
        print("  " + line)
    if failed:
        print("LUFA HID GATE: FAIL (static guards)")
        return 1
    with tempfile.TemporaryDirectory(prefix="lufa-hid-gate-") as tmp:
        rc, text = compile_checker(Path(tmp))
        if rc != 0:
            print("  FAIL " + text.strip())
            return 1
        proc = run_checker(text, USB_C, LAYOUT_TEST)
        sys.stdout.write(proc.stdout)
        sys.stderr.write(proc.stderr)
        if proc.returncode != 0:
            print("  FAIL LUFA HID check exited %d" % proc.returncode)
            return 1
    print("LUFA HID GATE: PASS (host-only, board-free, no firmware use)")
    return 0


def do_self_test() -> int:
    """Red canary: clean input must pass; four tampered inputs must fail."""
    if not USB_C.is_file() or not LAYOUT_TEST.is_file():
        print("LUFA HID SELF-TEST: FAIL missing shipped inputs", file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory(prefix="lufa-hid-selftest-") as tmp:
        build = Path(tmp)
        rc, text = compile_checker(build)
        if rc != 0:
            print("  FAIL compile: " + text.strip())
            return 1
        exe = text

        clean = run_checker(exe, USB_C, LAYOUT_TEST)
        print("  [%s] clean shipped inputs" % ("PASS" if clean.returncode == 0 else "FAIL"))
        if clean.returncode != 0:
            print(clean.stdout + clean.stderr)
            return 1

        usb_text = USB_C.read_text(encoding="utf-8")
        test_text = LAYOUT_TEST.read_text(encoding="utf-8")
        cases: List[Tuple[str, Path, Path]] = []

        bad_usb = build / "usb_key_tampered.c"
        bad_usb.write_text(usb_text.replace("#define USB_HID_KEY_A 0x04",
                                            "#define USB_HID_KEY_A 0x05", 1),
                           encoding="utf-8")
        cases.append(("tampered key code", bad_usb, LAYOUT_TEST))

        bad_item = build / "layout_item_tampered.c"
        bad_item.write_text(test_text.replace("0x05, 0x01, 0x09, 0x02, 0xA1, 0x01,",
                                              "0x0F, 0x01, 0x09, 0x02, 0xA1, 0x01,", 1),
                            encoding="utf-8")
        cases.append(("tampered descriptor item type", USB_C, bad_item))

        # 0x29 0x05 = "usage maximum: 5 buttons"; changing it to 6 must be
        # caught by the byte-for-byte canonical prefix comparison.
        bad_button = build / "layout_buttons_tampered.c"
        bad_button.write_text(test_text.replace("0x19, 0x01, 0x29, 0x05,",
                                                "0x19, 0x01, 0x29, 0x06,", 1),
                              encoding="utf-8")
        cases.append(("tampered button count", USB_C, bad_button))

        # 0x75 0x10 = "report size: 16 bits"; changing it to 8 must be caught
        # by decoding the fixture with the shipped decoder (X/Y width 16).
        bad_axis = build / "layout_axis_tampered.c"
        bad_axis.write_text(test_text.replace("0x75, 0x10, 0x95, 0x02,",
                                              "0x75, 0x08, 0x95, 0x02,", 1),
                            encoding="utf-8")
        cases.append(("tampered axis width", USB_C, bad_axis))

        all_red = True
        for label, usb, layout in cases:
            proc = run_checker(exe, usb, layout)
            ok = proc.returncode != 0
            all_red = all_red and ok
            print("  [%s] %s rejected" % ("PASS" if ok else "FAIL", label))
            if not ok:
                print("      " + proc.stdout.strip())

    print("LUFA HID SELF-TEST: %s (clean green, all canaries red)"
          % ("PASS" if all_red else "FAIL"))
    return 0 if all_red else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--check", action="store_true", help="run the gate")
    group.add_argument("--self-test", action="store_true", help="run red canaries")
    args = parser.parse_args()
    return do_check() if args.check else do_self_test()


if __name__ == "__main__":
    sys.exit(main())
