/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file engine3d_commands.c
 * @brief Batch-facing `gfx3d` verbs for P4MiniShell games.
 *
 * A thin facade over components/gfx3d: it binds the engine's injected
 * services to the PSRAM-first p4heap and the one DOS/CGA palette, attaches
 * to the single `gfx` canvas, and parses numeric vertex lists from argv.
 * It owns no raster code, no second surface and no second command dispatcher.
 * As with `gfx`, verbs are foreground-only (shared display surface) and never
 * auto-present: an app composes several verbs then calls `gfx show`.
 */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

#include "shell.h"
#include "batch.h"
#include "command.h"
#include "gfx.h"
#include "gfx3d.h"
#include "ansi.h"
#include "ansi_palette.h"
#include "p4minishell_config.h"
#include "p4heap.h"

/* Exported by components/command/gfx_commands.c: the ONE canvas accessor and
 * the ONE colour-argument parser. No private surface copy here. */
extern gfx_surface_t *gfx_canvas_surface(void);
extern bool gfx_canvas_is_open(void);
extern uint16_t gfx_canvas_parse_color(const char *s, uint16_t fallback);

static gfx3d_ctx_t *s_e3d = NULL;

/* ------------------------------------------------------------------------
 * Injected services: PSRAM-first bulk + the one DOS palette
 * ------------------------------------------------------------------------ */

static void *e3d_alloc(size_t size)
{
    return p4heap_alloc_psram(size);
}

static void e3d_release(void *ptr)
{
    p4heap_free(ptr);
}

static uint32_t e3d_dos_color(uint8_t index)
{
    return ansi_cga_color(index);
}

static uint8_t e3d_dos_nearest(uint32_t rgb)
{
    return ansi_cga_nearest(rgb);
}

static const gfx3d_ops_t s_e3d_ops = {
    e3d_alloc,
    e3d_release,
    e3d_dos_color,
    e3d_dos_nearest,
    NULL, /* no diagnostic sink on the firmware path */
    NULL,
};

static bool e3d_require_open(void)
{
    if (s_e3d != NULL && s_e3d->ready) return true;
    shell_transcript_appendf_ansi(SH_ERR "gfx3d: no engine (gfx3d init first)\n" SH_RST);
    batch_set_errorlevel(1);
    return false;
}

static bool e3d_require_canvas(void)
{
    if (gfx_canvas_is_open() && gfx_canvas_surface() != NULL) return true;
    shell_transcript_appendf_ansi(SH_ERR "gfx3d: no canvas (gfx init <w> <h> first)\n" SH_RST);
    batch_set_errorlevel(1);
    return false;
}

static bool e3d_require_foreground(void)
{
    if (batch_bg_is_background()) {
        shell_transcript_appendf_ansi(SH_ERR "gfx3d: not available in background jobs (shared display)\n" SH_RST);
        batch_set_errorlevel(1);
        return false;
    }
    return true;
}

/** Drop the engine and its buffers. Idempotent; the gfx canvas is untouched. */
static void e3d_teardown(void)
{
    if (s_e3d == NULL) return;
    gfx3d_close(s_e3d);
    p4heap_free(s_e3d);
    s_e3d = NULL;
}

/** Called by gfx_force_close's owner when a gfx session ends (wiring hook). */
void gfx3d_force_close(void)
{
    e3d_teardown();
}

static int e3d_axis(char c)
{
    if (c == 'x' || c == 'X') return 'x';
    if (c == 'y' || c == 'Y') return 'y';
    if (c == 'z' || c == 'Z') return 'z';
    return 0;
}

bool shell_command_engine3d(int argc, char **argv)
{
    if (argc < 2) {
        shell_transcript_appendf_ansi(SH_ERR "gfx3d: missing subcommand\n" SH_RST);
        shell_transcript_appendf_ansi(
            "Usage: gfx3d init | close | status | camera <ex ey ez tx ty tz ux uy uz> | "
            "project <fovy> <aspect_n> <aspect_d> <near> <far> |\n"
            "       gfx3d mesh <name> <color 0..15> <nv> <x y z ...> <nf> <i j k ...> | "
            "draw <name> | rotate <x|y|z> <deg> | translate <x y z> | "
            "scale <x y z> | light <x y z> [ambient 0..256] | "
            "mode <solid|wire> [cull|nocull] | clear [color] | stats | identity\n");
        shell_transcript_appendf_ansi(
            "       All coordinates are integers in world units; gfx3d draws into the open "
            "gfx canvas - call `gfx show` to present.\n");
        batch_set_errorlevel(2);
        return false;
    }

    if (shell_text_equals_ignore_case(argv[1], "init")) {
        if (!e3d_require_foreground()) return false;
        if (s_e3d != NULL) {
            shell_transcript_appendf_ansi(SH_ERR "gfx3d: already open (gfx3d close first)\n" SH_RST);
            batch_set_errorlevel(1);
            return false;
        }
        s_e3d = (gfx3d_ctx_t *)p4heap_alloc_psram(sizeof(*s_e3d));
        if (s_e3d == NULL || !gfx3d_init(s_e3d, &s_e3d_ops)) {
            p4heap_free(s_e3d);
            s_e3d = NULL;
            shell_transcript_appendf_ansi(SH_ERR "gfx3d init: out of memory\n" SH_RST);
            batch_set_errorlevel(1);
            return false;
        }
        shell_transcript_appendf("gfx3d: engine ready (verts=%d tris=%d, PSRAM)\n",
                                 GFX3D_MAX_VERTS, GFX3D_MAX_TRIS);
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "close")) {
        if (s_e3d == NULL) {
            shell_transcript_appendf_ansi(SH_ERR "gfx3d: not open\n" SH_RST);
            batch_set_errorlevel(1);
            return false;
        }
        e3d_teardown();
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "status")) {
        if (s_e3d == NULL) {
            shell_transcript_append_text("gfx3d: no engine\n");
        } else {
            int meshes = 0;
            int i;

            for (i = 0; i < GFX3D_MAX_MESHES; i++) {
                if (s_e3d->meshes[i].name[0] != '\0') meshes++;
            }
            shell_transcript_appendf(
                "gfx3d: ready=1 mode=%s cull=%d meshes=%d/%d near=%d far=%d "
                "shade_bytes=%u\n",
                s_e3d->mode == GFX3D_WIRE ? "wire" : "solid", s_e3d->cull,
                meshes, GFX3D_MAX_MESHES,
                gfx3d_to_int(s_e3d->near_z), gfx3d_to_int(s_e3d->far_z),
                (unsigned)s_e3d->stats.shade_bytes);
        }
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "stats")) {
        if (!e3d_require_open()) return false;
        shell_transcript_appendf(
            "gfx3d stats: verts=%d tris=%d clipped=%d culled=%d drawn=%d "
            "overflow=%d last_depth=%d\n",
            s_e3d->stats.verts, s_e3d->stats.tris, s_e3d->stats.clipped,
            s_e3d->stats.culled, s_e3d->stats.drawn, s_e3d->stats.overflow,
            gfx3d_to_int(s_e3d->stats.last_depth));
        batch_set_errorlevel(0);
        return true;
    }

    if (!e3d_require_open()) return false;

    if (shell_text_equals_ignore_case(argv[1], "camera")) {
        if (argc != 11) {
            shell_transcript_appendf_ansi(SH_ERR "gfx3d camera: usage: gfx3d camera <ex ey ez tx ty tz ux uy uz>\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        gfx3d_camera(s_e3d,
                     gfx3d_vec3(gfx3d_from_int(atoi(argv[2])),
                                gfx3d_from_int(atoi(argv[3])),
                                gfx3d_from_int(atoi(argv[4]))),
                     gfx3d_vec3(gfx3d_from_int(atoi(argv[5])),
                                gfx3d_from_int(atoi(argv[6])),
                                gfx3d_from_int(atoi(argv[7]))),
                     gfx3d_vec3(gfx3d_from_int(atoi(argv[8])),
                                gfx3d_from_int(atoi(argv[9])),
                                gfx3d_from_int(atoi(argv[10]))));
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "project")) {
        if (argc != 7) {
            shell_transcript_appendf_ansi(SH_ERR "gfx3d project: usage: gfx3d project <fovy> <aspect_n> <aspect_d> <near> <far>\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        gfx3d_projection(s_e3d, atoi(argv[2]), atoi(argv[3]), atoi(argv[4]),
                         gfx3d_from_int(atoi(argv[5])),
                         gfx3d_from_int(atoi(argv[6])));
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "rotate")) {
        int axis;

        if (argc != 4 || (axis = e3d_axis(argv[2][0])) == 0) {
            shell_transcript_appendf_ansi(SH_ERR "gfx3d rotate: usage: gfx3d rotate <x|y|z> <degrees>\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        gfx3d_model_rotate(s_e3d, (char)axis, atoi(argv[3]));
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "translate")) {
        if (argc != 5) {
            shell_transcript_appendf_ansi(SH_ERR "gfx3d translate: usage: gfx3d translate <x y z>\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        gfx3d_model_translate(s_e3d, gfx3d_from_int(atoi(argv[2])),
                              gfx3d_from_int(atoi(argv[3])),
                              gfx3d_from_int(atoi(argv[4])));
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "scale")) {
        if (argc != 5) {
            shell_transcript_appendf_ansi(SH_ERR "gfx3d scale: usage: gfx3d scale <x y z>\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        gfx3d_model_scale(s_e3d, gfx3d_from_int(atoi(argv[2])),
                          gfx3d_from_int(atoi(argv[3])),
                          gfx3d_from_int(atoi(argv[4])));
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "identity")) {
        gfx3d_model_identity(s_e3d);
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "light")) {
        int ambient = (argc > 5) ? atoi(argv[5]) : 256;

        if (argc != 5 && argc != 6) {
            shell_transcript_appendf_ansi(SH_ERR "gfx3d light: usage: gfx3d light <x y z> [ambient 0..256]\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        gfx3d_light(s_e3d,
                    gfx3d_vec3(gfx3d_from_int(atoi(argv[2])),
                               gfx3d_from_int(atoi(argv[3])),
                               gfx3d_from_int(atoi(argv[4]))),
                    ambient);
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "mode")) {
        int mode = GFX3D_SOLID;
        int cull = 1;

        if (argc < 3) {
            shell_transcript_appendf_ansi(SH_ERR "gfx3d mode: usage: gfx3d mode <solid|wire> [cull|nocull]\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        if (shell_text_equals_ignore_case(argv[2], "wire")) {
            mode = GFX3D_WIRE;
        } else if (!shell_text_equals_ignore_case(argv[2], "solid")) {
            shell_transcript_appendf_ansi(SH_ERR "gfx3d mode: bad mode '%s'\n" SH_RST, argv[2]);
            batch_set_errorlevel(2);
            return false;
        }
        if (argc > 3) {
            if (shell_text_equals_ignore_case(argv[3], "nocull")) {
                cull = 0;
            } else if (!shell_text_equals_ignore_case(argv[3], "cull")) {
                shell_transcript_appendf_ansi(SH_ERR "gfx3d mode: bad cull '%s'\n" SH_RST, argv[3]);
                batch_set_errorlevel(2);
                return false;
            }
        }
        gfx3d_mode(s_e3d, mode, cull);
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "mesh")) {
        gfx3d_fixed *vtmp = NULL;
        uint16_t *ftmp = NULL;
        int nv;
        int nf;
        int i;
        int ok;

        if (!e3d_require_foreground()) return false;
        if (argc < 6) {
            shell_transcript_appendf_ansi(SH_ERR "gfx3d mesh: usage: gfx3d mesh <name> <color 0..15> <nv> <x y z ...> <nf> <i j k ...>\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        nv = atoi(argv[4]);
        if (nv < 3 || nv > GFX3D_MAX_MESH_VERTS ||
            argc < 5 + 3 * nv + 1) {
            shell_transcript_appendf_ansi(SH_ERR "gfx3d mesh: bad vertex count %d (3..%d)\n" SH_RST,
                                          nv, GFX3D_MAX_MESH_VERTS);
            batch_set_errorlevel(2);
            return false;
        }
        nf = atoi(argv[5 + 3 * nv]);
        if (nf < 1 || nf > GFX3D_MAX_MESH_FACES ||
            argc != 6 + 3 * nv + 3 * nf) {
            shell_transcript_appendf_ansi(SH_ERR "gfx3d mesh: bad face count %d (1..%d) or token count\n" SH_RST,
                                          nf, GFX3D_MAX_MESH_FACES);
            batch_set_errorlevel(2);
            return false;
        }
        vtmp = (gfx3d_fixed *)p4heap_alloc_psram((size_t)nv * 3 * sizeof(*vtmp));
        ftmp = (uint16_t *)p4heap_alloc_psram((size_t)nf * 3 * sizeof(*ftmp));
        if (vtmp == NULL || ftmp == NULL) {
            p4heap_free(vtmp);
            p4heap_free(ftmp);
            shell_transcript_appendf_ansi(SH_ERR "gfx3d mesh: out of memory\n" SH_RST);
            batch_set_errorlevel(1);
            return false;
        }
        for (i = 0; i < nv * 3; i++) {
            vtmp[i] = gfx3d_from_int(atoi(argv[5 + i]));
        }
        for (i = 0; i < nf * 3; i++) {
            int index = atoi(argv[6 + 3 * nv + i]);

            if (index < 0 || index >= nv) index = 0;
            ftmp[i] = (uint16_t)index;
        }
        ok = gfx3d_mesh_create(s_e3d, argv[2], (uint8_t)atoi(argv[3]),
                               nv, vtmp, nf, ftmp);
        p4heap_free(vtmp);
        p4heap_free(ftmp);
        if (!ok) {
            shell_transcript_appendf_ansi(SH_ERR "gfx3d mesh: create failed (bounds or memory)\n" SH_RST);
            batch_set_errorlevel(1);
            return false;
        }
        shell_transcript_appendf("gfx3d: mesh %s %d vertices %d faces\n",
                                 argv[2], nv, nf);
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "draw")) {
        gfx3d_mesh_t *mesh;

        if (!e3d_require_foreground() || !e3d_require_canvas()) return false;
        if (argc != 3) {
            shell_transcript_appendf_ansi(SH_ERR "gfx3d draw: usage: gfx3d draw <name>\n" SH_RST);
            batch_set_errorlevel(2);
            return false;
        }
        mesh = gfx3d_mesh_find(s_e3d, argv[2]);
        if (mesh == NULL) {
            shell_transcript_appendf_ansi(SH_ERR "gfx3d draw: no mesh '%s'\n" SH_RST, argv[2]);
            batch_set_errorlevel(1);
            return false;
        }
        gfx3d_viewport(s_e3d, 0, 0, gfx_canvas_surface()->w,
                       gfx_canvas_surface()->h);
        gfx3d_projection(s_e3d, s_e3d->fovy_deg, s_e3d->aspect_num,
                         s_e3d->aspect_den, s_e3d->near_z, s_e3d->far_z);
        gfx3d_render(s_e3d, gfx_canvas_surface(), mesh);
        batch_set_errorlevel(0);
        return true;
    }

    if (shell_text_equals_ignore_case(argv[1], "clear")) {
        uint16_t color = (argc > 2) ? gfx_canvas_parse_color(argv[2], 0x0000)
                                    : 0x0000;

        if (!e3d_require_canvas()) return false;
        gfx3d_clear(gfx_canvas_surface(), color);
        batch_set_errorlevel(0);
        return true;
    }

    shell_transcript_appendf_ansi(SH_ERR "gfx3d: unknown subcommand '%s'\n" SH_RST, argv[1]);
    batch_set_errorlevel(2);
    return false;
}
