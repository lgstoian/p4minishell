#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""make_release.py - assemble the artifacts a public release ships.

This is the host-side counterpart to `release.md`: it drives the existing
pushers (never a second copy of their file lists), merges the firmware image,
archives the source tree, and writes a SHA256 manifest. It performs no
flashing itself; `push` delegates to the per-content pushers, which own the SD
layout.

Subcommands:
    bins     Merge bootloader + partition table + app into one flashable image
             via esptool, from an existing `idf.py` build directory.
    c6       Stage the per-board ESP32-C6 slave app images (esp32c6_hosted_slave-<board>.bin)
             from the coprocessor build dirs.
    srczip   `git archive` the committed tree into a clean source zip, leaving
             out the 430 MB refetchable managed_components/ (RESTORE.md says
             how to get them back from dependencies.lock).
    p4sync   Publish the self-contained Windows x64 P4Sync GUI and bundle it
             with the Python CLI/bridge + the harness modules it imports into
             p4sync-windows-x64-<version>.zip. No .NET runtime is required on
             the target PC; Python 3 + pyserial are.
    push     Run the SD pushers in order against a board (system apps, apps,
             assets, templates, dictionaries, fonts, the C6 image, packages).
    manifest Write release/SHA256SUMS over the artifacts in the output dir.
    all      bins + c6 + srczip + p4sync + manifest.

Usage:
    python tools/make_release.py bins   [--board jc1060p470c] [--build-dir build] [--out release] [--test-bin PATH]
    python tools/make_release.py c6     [--out release]
    python tools/make_release.py srczip [--out release]
    python tools/make_release.py p4sync [--out release] [--skip-publish]
    python tools/make_release.py push   COM6 [--sign NAME.priv.pem]
    python tools/make_release.py manifest [--out release]
    python tools/make_release.py all    [--board jc1060p470c] [--out release] [--skip-p4sync]

Run with the ESP-IDF environment active (the merge calls esptool). The build
directory must already exist (`idf.py -B <dir> -DP4_BOARD=<board> build`).
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Included at the archive root because the source zip leaves managed_components/
# out: the component manager refetches the pinned set, and the post-fetch
# patches are part of the source tree, not of the vendored drop.
RESTORE_MD = """# Restoring the ESP-IDF managed components

This source archive is the committed tree **without `managed_components/`**.
That directory is 430 MB of the ~480 MB checkout (about 10,700 files) and is
reproducible from `dependencies.lock`, so shipping it in every source zip only
makes the download more than five times larger (178 MB to about 29 MB).

`dependencies.lock` (kept at the repository root) pins every component name,
version and registry source. With an ESP-IDF 5.5.x environment active
(`export.ps1` on Windows, `. export.sh` elsewhere), run this from the directory
that contains this file:

    idf.py reconfigure

(or just `idf.py build`). The component manager downloads the pinned revisions
into `managed_components/`. Network access to components.espressif.com is
required for this step; later builds use the fetched copy.

A lock cut on a developer machine can contain `type: local` entries with an
absolute `path` (the archive keeps the lock byte-for-byte). If reconfigure
reports one of those paths as missing, remove that component's `source:` block
from `dependencies.lock` and rerun, or run `idf.py update-dependencies`;
`main/idf_component.yml` pins the same versions, so the manager fetches the
same releases from the registry.

The repository also carries small hand-authored patches on top of some fetched
components (LVGL async/PSRAM hardening, the button-matrix getter used by the
TUI touch verbs, the board BSP and esp_hosted fixes). Re-apply them after the
fetch:

    patch -p1 < tools/managed_patches.patch

(or `git apply tools/managed_patches.patch` in a checkout). On Windows,
`powershell -File tools/reapply_managed_patches.ps1` does the same and also
restores the extended `lv_font_unscii_16.c`; that font is a tracked file under
`managed_components/` in the full repository, so it is not in this archive.
Without it the fetched stock LVGL font is used, which still builds and runs but
draws a box in LVGL chrome for symbols outside its ranges. Copy it from the
source repository at the same tag if you need the extended glyph set.

Everything else - the board BSPs under `boards/`, the C6 co-processor project,
the Unity tests, `assets/fonts/`, every host tool, and the P4Sync host CLI/bridge
plus Windows WPF GUI source under `tools/p4sync/` and `apps/p4sync_gui/` - is a
committed file and is present in this archive. MSBuild `bin/`, `obj/` and
`build/` outputs are never included.
"""

# P4Sync integration: the GUI source is part of the committed tree, so git
# archive ships it. These pathspecs plus verify_srczip() keep MSBuild outputs
# out even if someone stages bin/obj by mistake.
SRC_ZIP_PREFIX = "p4minishell/"
SRC_ZIP_EXCLUDES = (
    ":(glob,exclude)apps/p4sync_gui/**/bin",
    ":(glob,exclude)apps/p4sync_gui/**/bin/**",
    ":(glob,exclude)apps/p4sync_gui/**/obj",
    ":(glob,exclude)apps/p4sync_gui/**/obj/**",
    ":(glob,exclude)apps/p4sync_gui/**/build",
    ":(glob,exclude)apps/p4sync_gui/**/build/**",
    ":(exclude)tools/p4sync/__pycache__",
)
SRC_ZIP_REQUIRED = (
    "apps/p4sync_gui/README.md",
    "apps/p4sync_gui/P4SyncGui.sln",
    "apps/p4sync_gui/src/P4SyncGui/P4SyncGui.csproj",
    "apps/p4sync_gui/src/P4SyncBridge/P4SyncBridge.csproj",
    "apps/p4sync_gui/tests/P4SyncBridge.Tests/P4SyncBridge.Tests.csproj",
    "tools/p4sync/README.md",
)

# Standalone Windows P4Sync bundle. The GUI is published self-contained (no
# .NET install needed on the PC); the Python side needs the bridge plus the
# harness modules it imports lazily. Credentials are NEVER copied.
P4SYNC_GUI_PROJECT = "apps/p4sync_gui/src/P4SyncGui/P4SyncGui.csproj"
P4SYNC_PY_FILES = (
    "tools/p4sync/__init__.py",
    "tools/p4sync/__main__.py",
    "tools/p4sync/protocol.py",
    "tools/p4sync/client.py",
    "tools/p4sync/bridge.py",
    "tools/p4sync/README.md",
    "tools/p4test/__init__.py",
    "tools/p4test/device.py",
    "tools/p4test/session.py",
    "tools/p4test/sdbridge.py",
    "tools/p4test/screenshot.py",
    "tools/p4test/local_config.py",
    "tools/p4test/panic_symbols.py",
    "tools/p4test/asserts.py",
    "tools/p4test/perf.py",
    "tools/p4test/runner.py",
    "tools/p4test/theme_guard.py",
    "tools/p4test/visual.py",
    "tools/p4test/visual_sweep.py",
    "tools/p4test/agent.py",
    "tools/p4test/sdbridge_selftest.py",
    "tools/pim_sync.py",
    "tools/board_ports.py",
    "tools/shell_session.py",
)
P4SYNC_BUNDLE_DOCS = (
    ("p4sync.md", "P4SYNC-GUIDE.md"),
    ("licence.md", "LICENCE.md"),
)
P4SYNC_REQUIRED_ENTRIES = (
    "P4SyncGui/P4SyncGui.exe",
    "tools/p4sync/bridge.py",
    "tools/p4sync/__main__.py",
    "tools/p4test/session.py",
    "tools/pim_sync.py",
    "tools/shell_session.py",
    "tools/board_ports.py",
    "P4SYNC-GUIDE.md",
    "README-FIRST.md",
    "run-p4sync.cmd",
)
P4SYNC_FORBIDDEN = ("wifi_local.json", ".priv.pem", "__pycache__", "/bin/", "/obj/", "\\bin\\", "\\obj\\")

P4SYNC_README = """P4Sync - Windows x64 standalone bundle
======================================

This bundle contains the self-contained P4Sync desktop GUI (no .NET install
needed) plus the Python CLI/bridge and the host-harness modules the bridge
imports. It talks to an attached P4MiniShell board over USB serial; it does not
flash or modify firmware.

Run the GUI
-----------
    run-p4sync.cmd

The launcher sets PYTHONPATH to the bundled tools/ directory and starts
P4SyncGui\\P4SyncGui.exe. No COM port is hardcoded: use the Devices page to
discover boards. For an offline demonstration, start the GUI with --mock:

    P4SyncGui\\P4SyncGui.exe --mock

Prerequisites
-------------
  * Windows 11 x64
  * Python 3.10+ with pyserial:  python -m pip install pyserial
  * A P4MiniShell board running the matching release firmware

Agent CLI (same bridge, machine-readable --json output)
-------------------------------------------------------
    python tools/p4sync --mock devices --json
    python tools/p4sync devices --json
    python tools/p4sync status --json --port COM3
    python tools/p4sync shell --json --port COM3 --command "ver"
    python tools/p4sync push --json --port COM3 --local FILE --remote sd:/APPS/FILE
    python tools/p4sync pull --json --port COM3 --remote sd:/APPS/FILE --local FILE
    python tools/p4sync screenshot --json --port COM3 --out shot.png

Full usage and screenshots: P4SYNC-GUIDE.md. Bridge contract and tests are in
tools/p4sync/README.md. Licences: LICENCE.md.

A --mock pass is a board-free demonstration, never hardware sign-off.
"""

P4SYNC_LAUNCHER = """@echo off
rem P4Sync standalone launcher (no .NET install required).
setlocal
set "P4SYNC_ROOT=%~dp0"
set "PYTHONPATH=%P4SYNC_ROOT%tools;%PYTHONPATH%"
start "" "%P4SYNC_ROOT%P4SyncGui\\P4SyncGui.exe" %*
endlocal
"""

# SD pushers, in the order content must land (system apps, then apps, then the
# asset manifests that describe them, then docs/fonts, then install bundles).
# Each owns its own layout; this list only sequences them.
PUSHERS = [
    ("apps/companion/push_sd.py", []),      # Companion + system BATs
    ("apps/push_apps.py", []),              # reference apps (root BAT + APPS/.APPINFO)
    ("apps/push_assets.py", []),            # generated assets + .ASSETS manifests
    ("apps/push_templates.py", []),         # sd:/TEMPLATES
    ("apps/push_dicts.py", []),             # sd:/DICTS
    ("push_fonts.py", []),                  # sd:/FONTS
    ("apps/push_c6.py", []),                # sd:/esp32c6_hosted_slave.bin (c6ota default)
    ("apps/push_pkgs.py", ["--sign"]),      # PKGS/<APP> bundles (sign optional)
]

BOARD_FLASH = {
    # board -> esptool --chip value for merge_bin (flash geometry comes from
    # the build's flash_args file)
    "jc1060p470c": "esp32p4",
    "m5stack_tab5": "esp32p4",
}


def log(msg):
    print("[release] " + msg)


def run(cmd, dry_run, cwd=ROOT):
    printable = " ".join(cmd)
    if dry_run:
        log("DRY-RUN: " + printable)
        return 0
    log(printable)
    return subprocess.call(cmd, cwd=cwd)


def cmd_bins(args):
    board = args.board
    if board not in BOARD_FLASH:
        log("unknown board %r (expected one of %s)" % (board, ", ".join(BOARD_FLASH)))
        return 2
    chip = BOARD_FLASH[board]
    build = os.path.join(ROOT, args.build_dir)
    flash_args = os.path.join(build, "flash_args")
    if not args.dry_run and not os.path.isfile(flash_args):
        log("no %s - build the project first: idf.py -B %s -DP4_BOARD=%s build"
            % (flash_args, args.build_dir, board))
        return 1

    out_dir = os.path.join(ROOT, args.out)
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, "p4minishell-%s.bin" % board)

    # build/flash_args already carries --flash_mode/--flash_freq/--flash_size
    # and build-relative file paths, so run esptool from the build directory
    # and pass only the chip plus the args file.
    rc = run([
        sys.executable, "-m", "esptool", "--chip", chip, "merge_bin",
        "-o", out, "@flash_args",
    ], args.dry_run, cwd=build)
    if rc != 0:
        log("merge_bin failed (is the ESP-IDF environment active?)")
        return rc
    log("wrote " + out)

    if args.test_bin and os.path.isfile(args.test_bin):
        dst = os.path.join(out_dir, "p4minishell-tests-%s.bin" % board)
        if args.dry_run:
            log("DRY-RUN: copy %s -> %s" % (args.test_bin, dst))
        else:
            shutil.copyfile(args.test_bin, dst)
            log("wrote " + dst)
    return 0


def cmd_c6(args):
    """Copy the per-board ESP32-C6 slave app images into the output dir.

    The images are produced by the coprocessor project (one build dir per
    board); this only stages them as release artifacts with distinct names.
    `push` also deploys the matching image to the SD card as
    `esp32c6_hosted_slave.bin` for `c6ota default`.
    """
    src_dir = os.path.join(ROOT, "coprocessor", "esp32c6_slave")
    out_dir = os.path.join(ROOT, args.out)
    os.makedirs(out_dir, exist_ok=True)
    rc = 0
    for board in BOARD_FLASH:
        src = os.path.join(src_dir, "build-%s" % board, "esp32c6_hosted_slave.bin")
        dst = os.path.join(out_dir, "esp32c6_hosted_slave-%s.bin" % board)
        if not os.path.isfile(src):
            log("missing %s - build it first: "
                "cd coprocessor/esp32c6_slave && idf.py -B build-%s -DC6_BOARD=%s build"
                % (src, board, board))
            rc = 1
            continue
        if args.dry_run:
            log("DRY-RUN: copy %s -> %s" % (src, dst))
        else:
            shutil.copyfile(src, dst)
            log("wrote " + dst)
    return rc


def verify_srczip(out):
    """Fail loudly if P4Sync GUI source is missing or MSBuild outputs shipped."""
    try:
        with zipfile.ZipFile(out) as archive:
            names = set(archive.namelist())
    except (OSError, zipfile.BadZipFile) as exc:
        log("could not inspect %s: %s" % (out, exc))
        return 1
    missing = [p for p in SRC_ZIP_REQUIRED if SRC_ZIP_PREFIX + p not in names]
    gui_prefix = SRC_ZIP_PREFIX + "apps/p4sync_gui/"
    bad = [name for name in names
           if name.startswith(gui_prefix)
           and any("/%s/" % part in name for part in ("bin", "obj", "build"))]
    if missing:
        log("source zip is missing committed P4Sync source: %s" % ", ".join(missing))
        return 1
    if bad:
        log("source zip ships MSBuild outputs: %s" % ", ".join(sorted(bad)[:10]))
        return 1
    log("source zip verified: P4Sync GUI source present, no bin/obj/build outputs")
    return 0


def cmd_srczip(args):
    out_dir = os.path.join(ROOT, args.out)
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, "p4minishell-src.zip")
    # git archive ships exactly the tracked tree (build/, sdkconfig, generated
    # files are git-ignored, so the zip is already clean). managed_components/
    # is tracked but refetchable from dependencies.lock: it is 430 MB of the
    # 480 MB tree, so the source zip leaves it out instead of tripling the
    # download (the repo still tracks it; only this artifact omits it).
    rc = run(["git", "archive", "--format=zip",
              "--prefix=p4minishell/", "-o", out, "HEAD",
              ":(exclude)managed_components", *SRC_ZIP_EXCLUDES], args.dry_run)
    if rc != 0:
        return rc

    # git archive only writes committed blobs, so RESTORE.md is added to the
    # archive (not to the working tree) as the one file that explains the
    # omission. Appending rewrites the central directory, which zipfile does;
    # the earlier entries stay byte-for-byte as git wrote them.
    dest = "p4minishell/RESTORE.md"
    if args.dry_run:
        log("DRY-RUN: add %s to %s" % (dest, out))
        log("DRY-RUN: verify %d P4Sync source paths present and no "
            "bin/obj/build outputs" % len(SRC_ZIP_REQUIRED))
        return 0
    try:
        with zipfile.ZipFile(out, "a", zipfile.ZIP_DEFLATED) as archive:
            archive.writestr(dest, RESTORE_MD)
    except (OSError, zipfile.BadZipFile) as exc:
        log("could not add %s: %s" % (dest, exc))
        return 1
    log("wrote %s (managed_components/ excluded, %s added)" % (out, dest))
    return verify_srczip(out)


def cmd_push(args):
    port = args.port
    for script, flags in PUSHERS:
        cmd = [sys.executable, script, port]
        if "--sign" in flags and args.sign:
            cmd += ["--sign", args.sign]
        rc = run(cmd, args.dry_run)
        if rc != 0:
            log("%s failed (rc=%d); stopping" % (script, rc))
            return rc
    log("all SD content pushed")
    return 0


def release_version():
    """Read the frozen P4_CONFIG_VERSION_* macros from the committed config."""
    path = os.path.join(ROOT, "p4minishell_config.h")
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    parts = []
    for macro in ("MAJOR", "MINOR", "PATCH"):
        match = re.search(r"#define\s+P4_CONFIG_VERSION_%s\s+(\d+)" % macro, text)
        if not match:
            raise RuntimeError("missing P4_CONFIG_VERSION_%s" % macro)
        parts.append(match.group(1))
    return "v" + ".".join(parts)


def cmd_p4sync(args):
    """Publish a standalone Windows x64 P4Sync bundle for the GitHub release."""
    out_dir = os.path.join(ROOT, args.out)
    if not args.dry_run and not os.path.isdir(out_dir):
        os.makedirs(out_dir)
    version = release_version()
    target = os.path.join(out_dir, "p4sync-windows-x64-%s.zip" % version)

    stage = tempfile.mkdtemp(prefix="p4sync-release-")
    try:
        gui_dir = os.path.join(stage, "P4SyncGui")
        if args.skip_publish:
            gui_dir = os.path.join(
                ROOT, "apps/p4sync_gui/src/P4SyncGui/bin/Release/net10.0-windows/win-x64/publish")
            log("reusing published GUI at %s" % gui_dir)
        else:
            rc = run(["dotnet", "publish", P4SYNC_GUI_PROJECT, "-c", "Release",
                      "-r", "win-x64", "--self-contained", "true", "-o", gui_dir],
                     args.dry_run)
            if rc != 0:
                log("dotnet publish failed (rc=%d); is the .NET 10 SDK installed?" % rc)
                return rc
        if not args.dry_run and not os.path.isfile(os.path.join(gui_dir, "P4SyncGui.exe")):
            log("no P4SyncGui.exe in the publish output")
            return 1

        if args.dry_run:
            log("DRY-RUN: write %s (self-contained GUI + Python bridge/harness)" % target)
            return 0

        with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
            if os.path.isdir(gui_dir):
                for folder, _dirs, files in os.walk(gui_dir):
                    for name in sorted(files):
                        full = os.path.join(folder, name)
                        arc = "P4SyncGui/" + os.path.relpath(full, gui_dir).replace(os.sep, "/")
                        zf.write(full, arc)
            for rel in P4SYNC_PY_FILES:
                src = os.path.join(ROOT, rel)
                if not os.path.isfile(src):
                    log("missing bundled source: %s" % rel)
                    return 1
                zf.write(src, rel)
            for src_rel, arc in P4SYNC_BUNDLE_DOCS:
                zf.write(os.path.join(ROOT, src_rel), arc)
            zf.writestr("README-FIRST.md", P4SYNC_README)
            zf.writestr("run-p4sync.cmd", P4SYNC_LAUNCHER.replace("\n", "\r\n"))

        with zipfile.ZipFile(target, "r") as zf:
            names = set(zf.namelist())
            missing = [entry for entry in P4SYNC_REQUIRED_ENTRIES if entry not in names]
            forbidden = [name for name in names
                         if any(bad in name for bad in P4SYNC_FORBIDDEN)]
        if missing or forbidden:
            if missing:
                log("bundle missing required entries: %s" % ", ".join(missing))
            if forbidden:
                log("bundle contains forbidden entries: %s" % ", ".join(forbidden[:5]))
            return 1
        digest = hashlib.sha256(open(target, "rb").read()).hexdigest()
        log("wrote %s (%d entries, sha256=%s)" % (target, len(names), digest[:16]))
        return 0
    finally:
        shutil.rmtree(stage, ignore_errors=True)


def cmd_manifest(args):
    out_dir = os.path.join(ROOT, args.out)
    if not os.path.isdir(out_dir):
        log("no %s directory yet" % args.out)
        return 1
    target = os.path.join(out_dir, "SHA256SUMS")
    lines = []
    for name in sorted(os.listdir(out_dir)):
        path = os.path.join(out_dir, name)
        if not os.path.isfile(path) or name == "SHA256SUMS":
            continue
        digest = hashlib.sha256(open(path, "rb").read()).hexdigest()
        lines.append("%s  %s\n" % (digest, name))
    if args.dry_run:
        log("DRY-RUN: write %d hashes to %s" % (len(lines), target))
        return 0
    with open(target, "w", newline="\n") as f:
        f.writelines(lines)
    log("wrote %s (%d entries)" % (target, len(lines)))
    return 0


def main():
    parser = argparse.ArgumentParser(description="Assemble release artifacts.")
    parser.add_argument("--dry-run", action="store_true",
                        help="print actions without executing")
    sub = parser.add_subparsers(dest="command")

    p_bins = sub.add_parser("bins", help="merge firmware bins")
    p_bins.add_argument("--board", default="jc1060p470c")
    p_bins.add_argument("--build-dir", default="build")
    p_bins.add_argument("--out", default="release")
    p_bins.add_argument("--test-bin", default=None,
                        help="optional test/ app bin to include")
    p_bins.set_defaults(func=cmd_bins)

    p_c6 = sub.add_parser("c6", help="stage the per-board ESP32-C6 slave images")
    p_c6.add_argument("--out", default="release")
    p_c6.set_defaults(func=cmd_c6)

    p_src = sub.add_parser("srczip", help="archive the source tree")
    p_src.add_argument("--out", default="release")
    p_src.set_defaults(func=cmd_srczip)

    p_p4sync = sub.add_parser("p4sync", help="bundle the Windows P4Sync binary")
    p_p4sync.add_argument("--out", default="release")
    p_p4sync.add_argument("--skip-publish", action="store_true",
                          help="reuse an existing self-contained publish directory")
    p_p4sync.set_defaults(func=cmd_p4sync)

    p_push = sub.add_parser("push", help="push SD content in order")
    p_push.add_argument("port")
    p_push.add_argument("--sign", default=None, help="ECDSA key for pkg bundles")
    p_push.set_defaults(func=cmd_push)

    p_man = sub.add_parser("manifest", help="write SHA256SUMS")
    p_man.add_argument("--out", default="release")
    p_man.set_defaults(func=cmd_manifest)

    p_all = sub.add_parser("all", help="bins + srczip + manifest")
    p_all.add_argument("--board", default="jc1060p470c")
    p_all.add_argument("--build-dir", default="build")
    p_all.add_argument("--out", default="release")
    p_all.add_argument("--test-bin", default=None)
    p_all.add_argument("--skip-p4sync", action="store_true",
                       help="skip the Windows P4Sync publish step")

    def cmd_all(args):
        # An 'all' run reuses the subcommand parsers' options.
        rc = cmd_bins(argparse.Namespace(
            board=args.board, build_dir=args.build_dir, out=args.out,
            test_bin=args.test_bin, dry_run=args.dry_run))
        if rc != 0:
            return rc
        rc = cmd_c6(argparse.Namespace(out=args.out, dry_run=args.dry_run))
        if rc != 0:
            return rc
        rc = cmd_srczip(argparse.Namespace(out=args.out, dry_run=args.dry_run))
        if rc != 0:
            return rc
        if not args.skip_p4sync:
            rc = cmd_p4sync(argparse.Namespace(
                out=args.out, dry_run=args.dry_run, skip_publish=False))
            if rc != 0:
                return rc
        return cmd_manifest(argparse.Namespace(out=args.out, dry_run=args.dry_run))

    p_all.set_defaults(func=cmd_all)

    args = parser.parse_args()
    if not getattr(args, "command", None):
        parser.print_help()
        return 2
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
