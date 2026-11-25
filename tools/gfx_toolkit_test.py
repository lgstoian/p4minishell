#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""gfx_toolkit_test.py - HW verification for the B2 gfx toolkit.

Runs the GFXTOOL.BAT reference app (raster primitives + on-canvas text),
pulls the saved GFXTOOL.BMP over `send`, and checks representative pixels
against the RGB565 quantization the firmware applies.

`--host` adds a board-free wiring/contract pre-gate for the additive toolkit
extension: it checks that every new pure surface function is declared,
defined and covered by a Unity case, that its verb is dispatched, and that
the GFXAA.BAT reference fixture still exercises it. It opens no serial port
and claims nothing about hardware (harness.md sect 0).

Usage: python tools/gfx_toolkit_test.py [COMx]
       python tools/gfx_toolkit_test.py --host
"""
import contextlib
import io
import json
import os
import re
import subprocess
import sys
import tempfile
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))
if TOOLS not in sys.path:
    sys.path.insert(0, TOOLS)
from bg_run import boot, run_quiet  # noqa: E402
from p4test.screenshot import TOL_EXACT, parse_bmp  # noqa: E402

FAILS = []


def q(rgb):
    """RGB888 -> RGB565 -> RGB888 (the firmware's quantization)."""
    r = (rgb >> 16) & 0xFF
    g = (rgb >> 8) & 0xFF
    b = rgb & 0xFF
    v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    return (((v >> 11) & 0x1F) << 3, ((v >> 5) & 0x3F) << 2, (v & 0x1F) << 3)


def check(name, got, want):
    ok = got == want
    print("  [%s] %s got=%s want=%s" % ("PASS" if ok else "FAIL", name, got, want))
    if not ok:
        FAILS.append(name)


# The BMP pixel model and its comparators are the ones in p4test.screenshot;
# a saved RGB565 frame is compared byte-exactly (TOL_EXACT), not with a private
# threshold or a second top-down reader.

ROOT = os.path.dirname(TOOLS)
GFX_FIXTURES = os.path.join(TOOLS, "host", "fixtures", "gfx")

# New pure surface entry point -> Unity case that asserts it. Keeping this map
# next to the checks makes a missing test as loud as a missing definition.
NEW_API = {
    "gfx_blend_565": "test_gfx_blend_565",
    "gfx_surface_line_aa": "test_gfx_line_aa",
    "gfx_surface_circle_aa": "test_gfx_circle_aa",
    "gfx_surface_rect_aa": "test_gfx_rect_aa",
    "gfx_surface_gradient": "test_gfx_gradient",
    "gfx_surface_blit_atlas": "test_gfx_blit_atlas",
    "gfx_surface_composite": "test_gfx_composite",
    # Bounded game toolkit: one atlas core, one deterministic RNG, one
    # composite primitive. Every entry point maps to a pure case in test_gfx.c.
    "gfx_tilemap_create": "gfx_case_tilemap",
    "gfx_tilemap_draw_all": "gfx_case_tilemap",
    "gfx_rand_next": "gfx_case_particles",
    "gfx_emitter_emit": "gfx_case_particles",
    "gfx_emitter_step": "gfx_case_particles",
    "gfx_emitter_draw": "gfx_case_particles",
    "gfx_layer_bind": "gfx_case_layers",
    "gfx_layer_mark_dirty": "gfx_case_layers",
    "gfx_layer_present": "gfx_case_layers",
}
# Image helpers: declaration/source/case live in different scoped files.
IMAGEFILE_API = {
    "imagefile_decode_fit": "imgdec_case_fit_decode",
    "imagefile_sample_rgb565_cells": "imgdec_case_cell_sampler",
}
NEW_VERBS = ["lineaa", "circleaa", "rectaa", "gradient", "composite"]
GAME_VERBS = ["tile", "particles", "layer"]
KEPT_VERBS = ["init", "close", "pixel", "line", "rect", "circle", "fill",
              "text", "blit", "blitmany", "save", "show"]


def read_text(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read()


def host_checks_texts(header, raster, verbs, tests, expected, fixture, extra):
    """Check one set of gfx sources; no file I/O, so canaries can tamper them.

    ``extra`` holds the image-side sources: test_imgdec.c, imagefile.h/.c and
    the one-sampler adapter header. Keeping them in one dict lets every
    canary mutate exactly one source at a time through this same checker.
    """
    for func, case in NEW_API.items():
        pattern = r"\b" + re.escape(func) + r"\s*\("
        check("gfx.h declares " + func, re.search(pattern, header) is not None, True)
        check("gfx.c defines " + func, re.search(pattern, raster) is not None, True)
        check("pure case " + case, re.search(r"\b" + re.escape(case) + r"\s*\(",
                                             tests) is not None, True)
    for verb in NEW_VERBS + GAME_VERBS:
        check("verb dispatch " + verb, '"' + verb + '"' in verbs, True)
    for verb in KEPT_VERBS:
        check("kept verb " + verb, '"' + verb + '"' in verbs, True)
    check("blitmany atlas flag", '"/a:"' in verbs, True)
    check("blitmany atlas blit", "gfx_surface_blit_atlas(&s_gfx" in verbs, True)
    check("composite plain layer", "gfx_surface_composite(&s_gfx" in verbs, True)
    check("composite keyed layer", "gfx_surface_composite_keyed(&s_gfx" in verbs, True)

    # Tilemap / particle / layer verbs must reach the one core primitives.
    for token in ("gfx_tilemap_create(&s_gfx_tilemaps",
                  "gfx_tilemap_draw_all(maps, count, &s_gfx",
                  "gfx_emitter_reset(s_gfx_particles",
                  "gfx_emitter_emit(s_gfx_particles",
                  "gfx_emitter_step(s_gfx_particles",
                  "gfx_layer_mark_dirty(&s_gfx_layers",
                  "gfx_layer_present_all(&s_gfx",
                  "gfx_toolkit_state_free();"):
        check("core call " + token, token in verbs, True)

    # Image side: one decoder, one sampler, one fit contract.
    for func, case in IMAGEFILE_API.items():
        pattern = r"\b" + re.escape(func) + r"\s*\("
        where = extra["cells_h"] if func == "imagefile_sample_rgb565_cells" \
                else extra["imagefile_h"]
        check("imagefile declares " + func, re.search(pattern, where) is not None, True)
        check("pure case " + case, re.search(r"\b" + re.escape(case) + r"\s*\(",
                                             extra["imgdec_tests"]) is not None, True)
    check("fit decode reuses one decoder",
          "imagefile_decode_mem(data, len, w, h, out)" in extra["imagefile_c"], True)
    check("no second PNG/JPEG decoder in imagefile",
          "lodepng" not in extra["imagefile_c"] and
          "tjpgd" not in extra["imagefile_c"], True)
    check("cell adapter delegates to one sampler",
          "tui_sample_image_cells(" in extra["cells_h"], True)
    check("cell adapter packs dom_image_t byte",
          "dos_fg << 4" in extra["cells_h"], True)

    for key in ("blend_midpoint", "blend_three_quarters_red", "line_aa_half_coverage",
                "gradient_one_third", "gradient_red_end", "gradient_blue_end",
                "keyed_composite_background"):
        check("host baseline " + key, expected[key] in tests, True)
    for key in ("tile_flip_x_pixel", "tile_rot90_pixel", "tile_hidden_pixel",
                "particle_step_x", "particle_step_y", "layer_dirty_origin",
                "layer_alpha_128_blend"):
        check("host baseline " + key, expected[key] in tests, True)
    for value in expected["atlas_cell_pixels"]:
        check("host baseline atlas " + value, value in tests, True)

    check("fixture start marker", "[M-GFXAA]" in fixture, True)
    check("fixture done marker", "[M-GFXAA-DONE]" in fixture, True)
    for verb in NEW_VERBS:
        check("fixture exercises " + verb, "gfx " + verb + " " in fixture, True)
    check("fixture atlas blitmany", "/a:1,16,16" in fixture, True)
    rich = extra["rich"]
    for marker in ("[M-GFXRICH]", "[M-GFXRICH-TILE]", "[M-GFXRICH-PARTICLE]",
                   "[M-GFXRICH-LAYER]", "[M-GFXRICH-DONE]"):
        check("rich fixture marker " + marker, marker in rich, True)
    for verb in ("gfx tile create ", "gfx tile set ", "gfx tile draw ",
                 "gfx particles seed ", "gfx particles emit ",
                 "gfx particles step", "gfx particles draw ",
                 "gfx layer bind ", "gfx layer dirty ", "gfx layer present "):
        check("rich fixture verb " + verb, verb in rich, True)

    print("\nRESULT %s (%d fail)" % ("OK" if not FAILS else "FAIL", len(FAILS)))
    return 0 if not FAILS else 1


def read_extra():
    """The image-side sources checked alongside the raster core."""
    return {
        "imgdec_tests": read_text(os.path.join(ROOT, "test", "main", "test_imgdec.c")),
        "imagefile_h": read_text(os.path.join(ROOT, "components", "imagefile", "imagefile.h")),
        "imagefile_c": read_text(os.path.join(ROOT, "components", "imagefile", "imagefile.c")),
        "cells_h": read_text(os.path.join(ROOT, "components", "imagefile", "imagefile_cells.h")),
        "rich": read_text(os.path.join(GFX_FIXTURES, "GFXRICH.BAT")),
    }


def host_checks():
    """Board-free wiring/contract pre-gate for the additive gfx toolkit."""
    print("gfx_toolkit_test: host wiring checks (no board)")
    header = read_text(os.path.join(ROOT, "components", "gfx", "gfx.h"))
    raster = read_text(os.path.join(ROOT, "components", "gfx", "gfx.c"))
    verbs = read_text(os.path.join(ROOT, "components", "command", "gfx_commands.c"))
    tests = read_text(os.path.join(ROOT, "test", "main", "test_gfx.c"))
    expected = json.loads(read_text(os.path.join(GFX_FIXTURES, "expected.json")))
    fixture = read_text(os.path.join(GFX_FIXTURES, "GFXAA.BAT"))
    return host_checks_texts(header, raster, verbs, tests, expected, fixture,
                             read_extra())


def host_self_test():
    """Red canaries: each new primitive must be seen missing when its wiring
    is removed from one source. Real files are only read, never modified.
    """
    global FAILS
    header = read_text(os.path.join(ROOT, "components", "gfx", "gfx.h"))
    raster = read_text(os.path.join(ROOT, "components", "gfx", "gfx.c"))
    verbs = read_text(os.path.join(ROOT, "components", "command", "gfx_commands.c"))
    tests = read_text(os.path.join(ROOT, "test", "main", "test_gfx.c"))
    expected = json.loads(read_text(os.path.join(GFX_FIXTURES, "expected.json")))
    fixture = read_text(os.path.join(GFX_FIXTURES, "GFXAA.BAT"))
    extra = read_extra()
    checks = []

    def run(name, expect_red=True, h=header, r=raster, v=verbs, t=tests,
            e=expected, f=fixture, x=extra):
        global FAILS
        FAILS = []
        with contextlib.redirect_stdout(io.StringIO()):
            rc = host_checks_texts(h, r, v, t, e, f, x)
        checks.append((name, (rc != 0 and len(FAILS) > 0) if expect_red
                       else (rc == 0 and not FAILS)))

    def mutated(**kwargs):
        out = dict(extra)
        out.update(kwargs)
        return out

    def rich_replace(old, new):
        return extra["rich"].replace(old, new)

    run("clean sources accepted", expect_red=False)
    run("missing AA declaration rejected", h=header.replace("gfx_surface_line_aa", "gfx_surface_line_xx"))
    run("missing AA definition rejected", r=raster.replace("gfx_surface_circle_aa", "gfx_surface_circle_xx"))
    run("missing Unity case rejected", t=tests.replace("test_gfx_gradient", "test_gfx_gradxx"))
    run("missing verb dispatch rejected", v=verbs.replace('"lineaa"', '"linexxx"'))
    run("removed atlas flag rejected", v=verbs.replace('"/a:"', '"/z:"'))
    run("removed keyed composite rejected",
        v=verbs.replace("gfx_surface_composite_keyed(&s_gfx", "gfx_surface_composite(&s_gfx"))
    run("missing baseline constant rejected",
        e={k: ("S28_UNKNOWN_BASELINE" if k == "line_aa_half_coverage" else val)
           for k, val in expected.items()})
    run("fixture without gradient rejected", f=fixture.replace("gfx gradient ", "gfx gradxxx "))
    run("fixture without atlas stamp rejected", f=fixture.replace("/a:1,16,16", "/x:1,16,16"))

    # Per-primitive canaries for the new toolkit.
    run("tilemap declaration rejected", h=header.replace("gfx_tilemap_create", "gfx_tilemap_creatX"))
    run("tilemap definition rejected", r=raster.replace("gfx_tilemap_draw_all", "gfx_tilemap_draw_alX"))
    run("tilemap pure case rejected", t=tests.replace("gfx_case_tilemap", "gfx_case_tilemaX"))
    run("tilemap verb rejected", v=verbs.replace('"tile"', '"tilX"'))
    run("particle RNG declaration rejected", h=header.replace("gfx_rand_next", "gfx_rand_nexX"))
    run("particle definition rejected", r=raster.replace("gfx_emitter_emit", "gfx_emitter_emiX"))
    run("particle pure case rejected", t=tests.replace("gfx_case_particles", "gfx_case_particule"))
    run("particle verb rejected", v=verbs.replace('"particles"', '"particleX"'))
    run("layer declaration rejected", h=header.replace("gfx_layer_mark_dirty", "gfx_layer_mark_dirtX"))
    run("layer definition rejected", r=raster.replace("gfx_layer_present", "gfx_layer_presenX"))
    run("layer pure case rejected", t=tests.replace("gfx_case_layers", "gfx_case_layerX"))
    run("layer verb rejected", v=verbs.replace('"layer"', '"layeX"'))
    run("fit decode declaration rejected",
        x=mutated(imagefile_h=extra["imagefile_h"].replace("imagefile_decode_fit", "imagefile_decode_fiX")))
    run("fit decode pure case rejected",
        x=mutated(imgdec_tests=extra["imgdec_tests"].replace("imgdec_case_fit_decode", "imgdec_case_fit_decodX")))
    run("sampler adapter delegation rejected",
        x=mutated(cells_h=extra["cells_h"].replace("tui_sample_image_cells(", "other_sample_image_cells(")))
    run("sampler pure case rejected",
        x=mutated(imgdec_tests=extra["imgdec_tests"].replace("imgdec_case_cell_sampler", "imgdec_case_cell_sampleX")))
    run("rich fixture tile marker rejected", f=rich_replace("[M-GFXRICH-TILE]", "[M-GFXRICH-TILX]"))
    run("rich fixture particle verb rejected", f=rich_replace("gfx particles emit ", "gfx particles emiX "))
    run("rich fixture layer marker rejected", f=rich_replace("[M-GFXRICH-LAYER]", "[M-GFXRICH-LAYEX]"))

    for name, ok in checks:
        print("  [%s] %s" % ("PASS" if ok else "FAIL", name))
    ok = bool(checks) and all(item[1] for item in checks)
    print("gfx_toolkit_test host self-test: %s (%d canaries)"
          % ("PASS" if ok else "FAIL", len(checks)))
    return 0 if ok else 1


def main():
    if "--self-test" in sys.argv[1:]:
        return host_self_test()
    if "--host" in sys.argv[1:]:
        return host_checks()
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    out = os.path.join(tempfile.gettempdir(), "p4minishell", "GFXTOOL.BMP")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    sh = boot(port)
    img = False
    try:
        run = run_quiet(sh, "GFXTOOL.BAT", timeout=45)
        check("marker start", "[M-GFXTOOL]" in run, True)
        check("marker done", "[M-GFXTOOL-DONE]" in run, True)
        check("saved", "gfx: saved" in run, True)
        check("image marker", ("[M-GFXTOOL-IMG]" in run) or
              ("[M-GFXTOOL-NOIMG]" in run), True)
        img = "[M-GFXTOOL-IMG]" in run
        if not img:
            print("  note: PHOTO.BMP absent - BMP blit region not checked")
    finally:
        sh.close()

    # Fetch the saved BMP with the proven pull driver (reboots; file is on SD).
    last = ""
    for _ in range(3):
        time.sleep(2)
        r = subprocess.run([sys.executable, os.path.join(TOOLS, "pull.py"),
                            "GFXTOOL.BMP", out, port],
                           capture_output=True, text=True)
        if "saved" in r.stdout:
            break
        last = "rc=%d out=%r err=%r" % (r.returncode, r.stdout[-200:], r.stderr[-200:])
    else:
        FAILS.append("pull")
        print("  [FAIL] pull GFXTOOL.BMP: %s" % last)
        return

    with open(out, "rb") as fh:
        b = parse_bmp(fh.read())
    print("  BMP %dx%d offset=%d" % (b.width, b.height, b.data_offset))
    check("dims", (b.width, b.height), (320, 200))
    check("background", b.px(10, 10), q(0x101820))
    check("border", b.px(4, 100), q(0x3A6EA5))
    check("triangle fill", b.px(40, 60), q(0xFF5555))
    check("ellipse fill", b.px(185, 95), q(0x55FF55))
    check("polygon fill", b.px(257, 70), q(0x5599FF))
    check("flood fill", b.px(266, 30), q(0xFFAA00))
    check("hline", b.px(100, 190), q(0xFFFFFF))
    check("vline", b.px(158, 100), q(0xFFFF00))
    check("text scale2 white",
          b.count_near(18, 150, 200 - 18, 166 - 150, q(0xFFFFFF),
                       tol=TOL_EXACT) > 0, True)
    check("text scale1 cyan",
          b.count_near(18, 170, 220 - 18, 178 - 170, q(0x00FFFF),
                       tol=TOL_EXACT) > 0, True)
    if img:
        # The scaled PHOTO.BMP blit lives in the lower-right free region; at
        # least one pixel there must differ from the background.
        check("bmp blit region", b.px(240, 160) != q(0x101820), True)

    print("\nRESULT %s (%d fail)" % ("OK" if not FAILS else "FAIL", len(FAILS)))
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main())
