#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""host_engine3d_gate.py - board-free engine3d render gate (pre-gate only).

Compiles the REAL shipped translation units (components/gfx3d/*.c, the ONE
components/gfx raster core and the ONE DOS palette in components/ansi) with
``tools/host/fixtures/engine3d/harness.c``, renders each numeric scene fixture
into a deterministic 24-bit BMP proof, and asserts geometry, colour and depth
order from the engine's own emit report and the one host pixel model
(``p4test.screenshot``). No second rasterizer, no fake backend and no serial
port are involved. The only host substitutions are the ESP allocator headers:
``p4heap_alloc_psram`` maps to malloc so the pure code links on a POSIX host.

A pass here is a pre-gate only. It never signs off hardware (harness.md §0/§2.2).

Usage:
    python tools/host_engine3d_gate.py              # --check (default)
    python tools/host_engine3d_gate.py --check
    python tools/host_engine3d_gate.py --update     # regenerate expected.json
    python tools/host_engine3d_gate.py --self-test  # red canaries
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
FIXTURES = TOOLS / "host" / "fixtures" / "engine3d"
EXPECTED = FIXTURES / "expected.json"
BASELINE_TAG = "engine3d-baseline-v1"

BEGIN = "e3d.render.begin"
END = "e3d.render.end"
FRAME = "e3d.frame path="
ROW_RE = re.compile(r"^e3d\.(tri|cull|draw|stats|overflow)\s+(.*)$")
KV_RE = re.compile(r"(\w+)=(\S+)")
INT_RE = re.compile(r"^-?\d+$")

# The host allocator shims. These replace only ESP heap declarations so the
# unmodified gfx/gfx3d/ansi sources compile with a plain C compiler; they do
# not implement, replace or approximate any rendering behaviour.
SHIM_HEADERS = {
    "esp_err.h": (
        "#ifndef ESP_ERR_H\n#define ESP_ERR_H\n"
        "typedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n#endif\n"
    ),
    "esp_heap_caps.h": (
        "#ifndef ESP_HEAP_CAPS_H\n#define ESP_HEAP_CAPS_H\n"
        "#include <stddef.h>\n#include <stdlib.h>\n"
        "#define MALLOC_CAP_8BIT 4\n#define MALLOC_CAP_SPIRAM 1024\n"
        "#define MALLOC_CAP_INTERNAL 2048\n#define MALLOC_CAP_DMA 8\n"
        "static inline void heap_caps_free(void*p){free(p);}\n"
        "static inline void*heap_caps_malloc(size_t n,unsigned c){(void)c;return malloc(n);}\n"
        "#endif\n"
    ),
    "p4heap.h": (
        "#ifndef P4HEAP_H\n#define P4HEAP_H\n"
        "#include <stddef.h>\n#include <stdlib.h>\n"
        "static inline void*p4heap_alloc_psram(size_t n){return malloc(n?n:1);}\n"
        "static inline void*p4heap_calloc_psram(size_t n,size_t s){return calloc(n?n:1,s?s:1);}\n"
        "static inline void*p4heap_realloc_psram(void*p,size_t n){return realloc(p,n);}\n"
        "static inline void*p4heap_alloc_any(size_t n){return malloc(n?n:1);}\n"
        "static inline void*p4heap_calloc_any(size_t n,size_t s){return calloc(n?n:1,s?s:1);}\n"
        "static inline void p4heap_free(void*p){free(p);}\n#endif\n"
    ),
}

SOURCES = (
    "components/gfx3d/gfx3d_math.c",
    "components/gfx3d/gfx3d_core.c",
    "components/gfx/gfx.c",
    "components/gfx/gfx_font.c",
    "components/ansi/ansi.c",
)


class GateError(RuntimeError):
    """A condition that must make the gate red."""


@dataclass
class Parsed:
    fixture: str
    width: int
    height: int
    lines: List[str] = field(default_factory=list)
    tri_ids: List[int] = field(default_factory=list)
    draw_ids: List[int] = field(default_factory=list)
    draw_depths: List[int] = field(default_factory=list)
    draw_colors: List[str] = field(default_factory=list)
    cull_ids: List[int] = field(default_factory=list)
    stats: Dict[str, int] = field(default_factory=dict)
    end: Dict[str, int] = field(default_factory=dict)
    frame: Optional[str] = None


def kv(line: str) -> Dict[str, str]:
    return dict(KV_RE.findall(line))


def parse_report(raw: str, fixture: str) -> Parsed:
    """Normalise harness stdout, reject stale/vacuous reports, count assertions."""
    text = raw.replace("\r\n", "\n").replace("\r", "\n")
    begin = end = None
    parsed = Parsed(fixture=fixture, width=0, height=0)
    seen_frame = False

    for line in text.split("\n"):
        line = line.rstrip()
        if line.startswith(BEGIN):
            begin = line
        elif line.startswith(END):
            end = line
        elif line.startswith(FRAME):
            if seen_frame:
                raise GateError("duplicate frame line")
            seen_frame = True
            parsed.frame = line[len(FRAME):].strip()
            if not parsed.frame:
                raise GateError("empty frame path")
            parsed.lines.append("e3d.frame path=" + Path(parsed.frame).name)
        else:
            match = ROW_RE.match(line)
            if match is None:
                continue
            kind, body = match.group(1), match.group(2)
            fields = kv(body)
            parsed.lines.append("e3d.%s %s" % (kind, body))
            if kind == "tri":
                parsed.tri_ids.append(int(fields["id"]))
            elif kind == "cull":
                parsed.cull_ids.append(int(fields["id"]))
            elif kind == "draw":
                parsed.draw_ids.append(int(fields["id"]))
                parsed.draw_depths.append(int(fields["depth"]))
                parsed.draw_colors.append(fields["color"])
            elif kind == "stats":
                parsed.stats = {k: int(v) for k, v in fields.items()
                                if INT_RE.match(v)}

    if begin is None or end is None:
        raise GateError("stale/missing begin/end marker for %s" % fixture)
    b, e = kv(begin), kv(end)
    if b.get("fixture") != fixture or e.get("fixture") != fixture:
        raise GateError("stale marker fixture mismatch for %s" % fixture)
    try:
        parsed.width = int(b["w"])
        parsed.height = int(b["h"])
        parsed.end = {k: int(v) for k, v in e.items() if k != "fixture"}
    except (KeyError, ValueError) as exc:
        raise GateError("unreadable begin/end counts for %s: %s" % (fixture, exc))
    if parsed.width < 1 or parsed.height < 1:
        raise GateError("degenerate canvas for %s" % fixture)
    if parsed.frame is None:
        raise GateError("no BMP proof for %s" % fixture)
    if not parsed.lines or not parsed.stats:
        raise GateError("zero assertions: no rows/stats emitted for %s" % fixture)
    if parsed.end.get("draws") != len(parsed.draw_ids):
        raise GateError("stale draw count for %s: end=%s emitted=%d"
                        % (fixture, parsed.end.get("draws"), len(parsed.draw_ids)))
    if parsed.end.get("culls") != len(parsed.cull_ids):
        raise GateError("stale cull count for %s: end=%s emitted=%d"
                        % (fixture, parsed.end.get("culls"), len(parsed.cull_ids)))
    if parsed.end.get("overflow") != parsed.stats.get("overflow"):
        raise GateError("stale overflow count for %s: end=%s stats=%s"
                        % (fixture, parsed.end.get("overflow"),
                           parsed.stats.get("overflow")))
    if len(set(parsed.draw_ids)) != len(parsed.draw_ids):
        raise GateError("double raster: a triangle id was drawn more than once in %s"
                        % fixture)
    return parsed


def check_report(parsed: Parsed, want: dict) -> int:
    """Geometry/colour/depth assertions on the real core's own report."""
    assertions = 0
    name = parsed.fixture

    if (parsed.width, parsed.height) != tuple(want["canvas"]):
        raise GateError("%s canvas %dx%d, want %s"
                        % (name, parsed.width, parsed.height, want["canvas"]))
    assertions += 1
    for key in ("tris", "culled", "drawn", "overflow"):
        got = parsed.stats.get(key)
        expected = want[key] if key != "drawn" else len(want["draws"])
        if got != expected:
            raise GateError("%s stats %s=%s, want %s" % (name, key, got, expected))
        assertions += 1
    if parsed.draw_ids != list(want["draws"]):
        raise GateError("%s draw order %s, want %s"
                        % (name, parsed.draw_ids, want["draws"]))
    assertions += 1
    if parsed.cull_ids != list(want["culls"]):
        raise GateError("%s cull ids %s, want %s"
                        % (name, parsed.cull_ids, want["culls"]))
    assertions += 1
    # Painter's order: depth is a distance, so the queue must be non-increasing
    # (farthest first). A stale/reversed depth sort cannot pass this.
    for i in range(1, len(parsed.draw_depths)):
        if parsed.draw_depths[i] > parsed.draw_depths[i - 1]:
            raise GateError("%s stale depth order: %s"
                            % (name, parsed.draw_depths))
        assertions += 1
    return assertions


def _load_pixel_model():
    tools = str(TOOLS)
    if tools not in sys.path:
        sys.path.insert(0, tools)
    try:
        from p4test import screenshot  # noqa: E402
    except Exception as exc:  # noqa: BLE001
        raise GateError("BMP proof needs the one p4test pixel model: %s" % exc)
    return screenshot


def check_frame(parsed: Parsed, want: dict, frames_dir: Path) -> Tuple[List[str], int]:
    """Pixel assertions through the ONE host BMP reader, never a private decoder."""
    screenshot = _load_pixel_model()
    path = frames_dir / Path(parsed.frame).name
    if not path.is_file():
        raise GateError("%s BMP proof missing: %s" % (parsed.fixture, path))
    bmp = screenshot.parse_bmp(path.read_bytes())
    if (bmp.width, bmp.height) != tuple(want["canvas"]):
        raise GateError("%s BMP is %dx%d, want %s"
                        % (parsed.fixture, bmp.width, bmp.height, want["canvas"]))
    notes = ["frame %s: %dx%d" % (path.name, bmp.width, bmp.height)]
    assertions = 1

    for coord, rgb in want.get("pixels", {}).items():
        x, y = (int(v) for v in coord.split(","))
        got = bmp.px(x, y)
        if got != tuple(rgb):
            raise GateError("%s pixel %s=%s, want %s"
                            % (parsed.fixture, coord, got, tuple(rgb)))
        assertions += 1
    ink = sum(1 for y in range(bmp.height) for x in range(bmp.width)
              if bmp.px(x, y) != (0, 0, 0))
    if ink < int(want.get("ink_min", 0)):
        raise GateError("%s ink=%d, want >= %d"
                        % (parsed.fixture, ink, want["ink_min"]))
    assertions += 1
    notes.append("ink=%d" % ink)
    for rgb in want.get("color_any", []):
        if not any(bmp.px(x, y) == tuple(rgb)
                   for y in range(bmp.height) for x in range(bmp.width)):
            raise GateError("%s has no pixel of %s" % (parsed.fixture, tuple(rgb)))
        assertions += 1
    return notes, assertions


def evaluate(raw: str, fixture: str, want: dict, frames_dir: Path
             ) -> Tuple[List[str], int]:
    parsed = parse_report(raw, fixture)
    assertions = check_report(parsed, want)
    notes, pixel_assertions = check_frame(parsed, want, frames_dir)
    return notes, assertions + pixel_assertions


def wsl_path(path: Path) -> str:
    text = str(path.resolve()).replace("\\", "/")
    match = re.match(r"^([A-Za-z]):/(.*)$", text)
    if match:
        return "/mnt/%s/%s" % (match.group(1).lower(), match.group(2))
    return text


def host_compiler() -> Tuple[List[str], bool]:
    """Return (argv prefix, needs_wsl_paths)."""
    cc = shutil.which("gcc") or shutil.which("cc")
    if cc:
        return [cc], False
    wsl = shutil.which("wsl.exe") or shutil.which("wsl")
    if wsl:
        return [wsl, "--exec", "gcc"], True
    raise GateError("no C compiler found (gcc/cc on PATH, or wsl.exe with gcc)")


def compile_harness(build: Path) -> Path:
    """Compile the real core + harness with honest host allocator shims."""
    build.mkdir(parents=True, exist_ok=True)
    inc = build / "inc"
    inc.mkdir(parents=True, exist_ok=True)
    for name, body in SHIM_HEADERS.items():
        (inc / name).write_text(body, encoding="utf-8", newline="\n")
    cc, wsl = host_compiler()
    exe = build / ("host_engine3d_render.exe" if os.name == "nt" and not wsl
                   else "host_engine3d_render")

    def p(path: Path) -> str:
        return wsl_path(path) if wsl else str(path)

    cmd = list(cc) + [
        "-std=c11", "-O0", "-Wall",
        "-I" + p(inc),
        "-I" + p(ROOT / "components" / "gfx3d"),
        "-I" + p(ROOT / "components" / "gfx"),
        "-I" + p(ROOT / "components" / "ansi"),
        "-I" + p(ROOT),
        p(FIXTURES / "harness.c"),
    ] + [p(ROOT / src) for src in SOURCES] + [
        "-lm", "-o", p(exe),
    ]
    try:
        proc = subprocess.run(cmd, cwd=str(ROOT), capture_output=True,
                              text=True, errors="replace", timeout=180)
    except OSError as exc:
        raise GateError("cannot invoke the C compiler: %s" % exc)
    except subprocess.TimeoutExpired:
        raise GateError("harness compile timed out")
    if proc.returncode != 0:
        raise GateError("harness compile failed:\n%s" %
                        (proc.stderr or proc.stdout)[-2000:])
    return exe


def run_harness(exe: Path, fixture: Path, frames: Path, wsl: bool) -> str:
    frames.mkdir(parents=True, exist_ok=True)
    cc_prefix_wsl = wsl
    cmd = ([wsl_path(exe)] if cc_prefix_wsl else [str(exe)])
    cmd += [wsl_path(fixture) if cc_prefix_wsl else str(fixture),
            wsl_path(frames) if cc_prefix_wsl else str(frames)]
    try:
        proc = subprocess.run(cmd, cwd=str(ROOT), capture_output=True,
                              text=True, encoding="utf-8", errors="replace",
                              timeout=60)
    except OSError as exc:
        raise GateError("cannot run the harness: %s" % exc)
    except subprocess.TimeoutExpired:
        raise GateError("harness timed out on %s" % fixture.name)
    if proc.returncode != 0:
        raise GateError("harness exited %d on %s: %s"
                        % (proc.returncode, fixture.name,
                           (proc.stderr or "")[-400:]))
    return proc.stdout


BATCH_VERBS = ("init", "close", "status", "camera", "mesh", "draw",
               "rotate", "project", "clear", "stats",
               "translate", "scale", "light", "mode", "identity")


def check_batch_fixture(text: str) -> int:
    """The shipped .BAT proves every verb is usable from a batch/SD app."""
    assertions = 0
    if "[M-ENGINE3D]" not in text or "[M-ENGINE3D-DONE]" not in text:
        raise GateError("batch fixture markers missing")
    assertions += 2
    for verb in BATCH_VERBS:
        if re.search(r"\bgfx3d\s+" + re.escape(verb) + r"\b", text) is None:
            raise GateError("batch fixture never uses `gfx3d %s`" % verb)
        assertions += 1
    return assertions


def load_expected() -> dict:
    if not EXPECTED.is_file():
        raise GateError("missing baseline %s (run --update)" % EXPECTED)
    doc = json.loads(EXPECTED.read_text(encoding="utf-8"))
    if doc.get("version") != 1 or "fixtures" not in doc:
        raise GateError("stale baseline marker in %s" % EXPECTED)
    return doc["fixtures"]


def run_gate(update: bool) -> int:
    expected = load_expected()
    batch_assertions = check_batch_fixture(
        (FIXTURES / "ENGINE3D.BAT").read_text(encoding="utf-8", errors="replace"))
    fixtures = sorted(FIXTURES.glob("*.e3d"))
    if not fixtures:
        raise GateError("no .e3d fixtures in %s" % FIXTURES)
    missing = [f.stem for f in fixtures if f.stem not in expected]
    if missing:
        raise GateError("fixtures without baseline: %s" % ", ".join(missing))

    with tempfile.TemporaryDirectory(prefix="engine3d_gate_") as tmp:
        work = Path(tmp)
        exe = compile_harness(work / "build")
        _, wsl = host_compiler()
        frames = work / "frames"
        total = 0
        for fixture in fixtures:
            want = dict(expected[fixture.stem])
            raw = run_harness(exe, fixture, frames, wsl)
            if update:
                # Regenerate from the engine's own report and BMP proof.
                parsed = parse_report(raw, fixture.stem)
                check_report(parsed, want)
                want["canvas"] = [parsed.width, parsed.height]
                want["draws"] = parsed.draw_ids
                want["culls"] = parsed.cull_ids
                want["tris"] = parsed.stats["tris"]
                want["culled"] = parsed.stats["culled"]
                want["overflow"] = parsed.stats["overflow"]
                screenshot = _load_pixel_model()
                proof = frames / Path(parsed.frame).name
                bmp = screenshot.parse_bmp(proof.read_bytes())
                want["pixels"] = {
                    coord: list(bmp.px(*(int(v) for v in coord.split(","))))
                    for coord in want.get("pixels", {})
                }
                want["ink_min"] = sum(
                    1 for y in range(bmp.height) for x in range(bmp.width)
                    if bmp.px(x, y) != (0, 0, 0))
                expected[fixture.stem] = want
                print("  updated %s" % fixture.stem)
                continue
            notes, assertions = evaluate(raw, fixture.stem, want, frames)
            total += assertions
            for note in notes:
                print("    " + note)
            draws = len(re.findall(r"^e3d\.draw ", raw, re.M))
            culls = len(re.findall(r"^e3d\.cull ", raw, re.M))
            match = re.search(r"overflow=(\d+)", raw)
            overflow = int(match.group(1)) if match else -1
            print("  %s: %d draw(s), %d cull(s), overflow=%d, %d assertion(s)"
                  % (fixture.stem, draws, culls, overflow, assertions))
        if update:
            body = {
                "version": 1,
                "note": "Baselines emitted by the real gfx3d core through "
                        "tools/host/fixtures/engine3d/harness.c. Regenerate "
                        "with: python tools/host_engine3d_gate.py --update",
                "fixtures": expected,
            }
            EXPECTED.write_text(json.dumps(body, indent=2) + "\n",
                                encoding="utf-8")
            print("engine3d gate UPDATE: rewrote %s" % EXPECTED.name)
            return 0
    total += batch_assertions
    print("engine3d gate CHECK: %d fixture(s), %d assertion(s) -> OK"
          % (len(fixtures), total))
    return 0


# ---------------------------------------------------------------------------
# Red canaries
# ---------------------------------------------------------------------------

def good_report() -> str:
    return "\n".join([
        BEGIN + " fixture=depth w=64 h=64",
        "e3d.tri id=0 depth=4 shade=7 color=faaa",
        "e3d.tri id=1 depth=8 shade=7 color=52bf",
        "e3d.draw id=1 depth=8 color=52bf sx=40,24,32 sy=40,40,24",
        "e3d.draw id=0 depth=4 color=faaa sx=48,16,32 sy=48,48,16",
        "e3d.stats verts=6 tris=2 clipped=0 culled=0 drawn=2 overflow=0 "
        "last_depth=4 shade_bytes=256",
        FRAME + "depth.bmp",
        END + " fixture=depth draws=2 culls=0 overflow=0",
    ]) + "\n"


def good_want() -> dict:
    return {
        "canvas": [64, 64], "tris": 2, "culled": 0, "draws": [1, 0],
        "culls": [], "overflow": 0, "pixels": {}, "ink_min": 0,
        "color_any": [],
    }


def cull_report() -> str:
    """A report where the back face was correctly culled."""
    return "\n".join([
        BEGIN + " fixture=cull w=64 h=64",
        "e3d.tri id=0 depth=8 shade=7 color=faaa",
        "e3d.cull id=1 area=-256 sx=40,32,24 sy=40,24,40",
        "e3d.draw id=0 depth=8 color=faaa sx=40,24,32 sy=40,40,24",
        "e3d.stats verts=6 tris=2 clipped=0 culled=1 drawn=1 overflow=0 "
        "last_depth=8 shade_bytes=256",
        FRAME + "cull.bmp",
        END + " fixture=cull draws=1 culls=1 overflow=0",
    ]) + "\n"


def make_bmp(w: int, h: int, rgb) -> bytes:
    """Minimal bottom-up 24-bit BMP for the canary pixel checks."""
    import struct
    stride = (w * 3 + 3) & ~3
    row = bytes(rgb) * w + b"\x00" * (stride - w * 3)
    payload = row * h
    header = b"BM" + struct.pack("<IHHI", 54 + len(payload), 0, 0, 54)
    header += struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, len(payload),
                          0, 0, 0, 0)
    return header + payload


def self_test() -> int:
    checks: List[Tuple[str, bool]] = []
    good = good_report()
    want = good_want()
    with tempfile.TemporaryDirectory(prefix="engine3d_selftest_") as tmp:
        frames = Path(tmp)
        (frames / "depth.bmp").write_bytes(make_bmp(64, 64, (20, 20, 20)))

        def accepts(raw: str, w: dict = want, fixture: str = "depth") -> bool:
            try:
                evaluate(raw, fixture, w, frames)
                return True
            except GateError:
                return False

        checks.append(("clean report accepted", accepts(good)))

        # Double raster: the same triangle id drawn twice must go red.
        doubled = good.replace(
            "e3d.draw id=0 depth=4 color=faaa sx=48,16,32 sy=48,48,16\n",
            "e3d.draw id=0 depth=4 color=faaa sx=48,16,32 sy=48,48,16\n"
            "e3d.draw id=0 depth=4 color=faaa sx=48,16,32 sy=48,48,16\n"
        ).replace(END + " fixture=depth draws=2", END + " fixture=depth draws=3")
        checks.append(("double raster rejected", not accepts(doubled)))

        # No cull: the back face is painted instead of being culled. The
        # expected cull report keeps id=1 hidden; painting it must go red.
        cull_good = cull_report()
        (frames / "cull.bmp").write_bytes(make_bmp(64, 64, (20, 20, 20)))
        cull_want = dict(want, tris=2, culled=1, draws=[0], culls=[1])
        checks.append(("cull report accepted",
                       accepts(cull_good, cull_want, "cull")))
        nocull = cull_good.replace(
            "e3d.cull id=1 area=-256 sx=40,32,24 sy=40,24,40\n", "")
        nocull = nocull.replace(
            "e3d.draw id=0 depth=8 color=faaa sx=40,24,32 sy=40,40,24\n",
            "e3d.draw id=1 depth=8 color=faaa sx=40,24,32 sy=40,40,24\n"
            "e3d.draw id=0 depth=8 color=faaa sx=40,24,32 sy=40,40,24\n")
        nocull = nocull.replace("culled=1 drawn=1", "culled=0 drawn=2")
        nocull = nocull.replace("draws=1 culls=1", "draws=2 culls=0")
        checks.append(("no-cull report rejected",
                       not accepts(nocull, cull_want, "cull")))

        # Stale depth: the near triangle is presented before the far one.
        stale = good.replace(
            "e3d.draw id=1 depth=8 color=52bf sx=40,24,32 sy=40,40,24\n"
            "e3d.draw id=0 depth=4 color=faaa sx=48,16,32 sy=48,48,16\n",
            "e3d.draw id=0 depth=4 color=faaa sx=48,16,32 sy=48,48,16\n"
            "e3d.draw id=1 depth=8 color=52bf sx=40,24,32 sy=40,40,24\n")
        checks.append(("stale depth order rejected", not accepts(stale)))

        # Buffer overflow: a nonzero overflow count must be reported and fail.
        overflow = good.replace("drawn=2 overflow=0", "drawn=2 overflow=1")
        checks.append(("buffer overflow rejected", not accepts(overflow)))

        # Zero assertions / stale marker / wrong pixel.
        vacuous = "\n".join([BEGIN + " fixture=depth w=64 h=64",
                             FRAME + "depth.bmp",
                             END + " fixture=depth draws=0 culls=0 overflow=0"])
        checks.append(("zero assertions rejected", not accepts(vacuous)))
        checks.append(("missing marker rejected",
                       not accepts(good.replace(END + " fixture=depth "
                                                "draws=2 culls=0 overflow=0", ""))))
        bad_pixel = dict(want)
        bad_pixel["pixels"] = {"10,10": [255, 0, 0]}
        checks.append(("wrong pixel baseline rejected", not accepts(good, bad_pixel)))
        checks.append(("ink floor rejected",
                       not accepts(good, dict(want, ink_min=9999))))

        batch_text = (FIXTURES / "ENGINE3D.BAT").read_text(encoding="utf-8")
        try:
            check_batch_fixture(batch_text)
            checks.append(("batch fixture accepted", True))
        except GateError:
            checks.append(("batch fixture accepted", False))
        try:
            check_batch_fixture(batch_text.replace("gfx3d rotate ", "gfx3d rotatX "))
            checks.append(("missing batch verb rejected", False))
        except GateError:
            checks.append(("missing batch verb rejected", True))

    for name, ok in checks:
        print("  [%s] %s" % ("PASS" if ok else "FAIL", name))
    ok = bool(checks) and all(item[1] for item in checks)
    print("engine3d gate self-test: %s (%d canaries)"
          % ("PASS" if ok else "FAIL", len(checks)))
    return 0 if ok else 1


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true",
                      help="compare fixtures against baselines (default)")
    mode.add_argument("--update", action="store_true",
                      help="regenerate expected.json from the current core")
    ap.add_argument("--self-test", action="store_true",
                    help="run the red-canary self-test")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    try:
        return run_gate(args.update)
    except GateError as exc:
        print("engine3d gate: FAIL - %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
