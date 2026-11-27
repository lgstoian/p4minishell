/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file test_engine3d.c
 * @brief Pure unit tests for the mini 3D engine (math, cull, clip, depth, shade).
 *
 * These cases drive the real gfx3d core with an injected malloc allocator and
 * the one DOS/CGA palette; pixels land in a small local gfx_surface_t through
 * gfx_surface_triangle()/line(), the same raster path the firmware uses.
 */
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "gfx.h"
#include "gfx3d.h"
#include "ansi.h"

#define TEST_W 32
#define TEST_H 32

static char      s_emit[8192];
static size_t    s_emit_len;
static uint16_t  s_px[TEST_W * TEST_H];
static gfx_surface_t s_surface;

static void t_emit(const char *line, void *ud)
{
    size_t n = strlen(line);
    (void)ud;
    if (s_emit_len + n + 2 >= sizeof(s_emit)) return;
    memcpy(s_emit + s_emit_len, line, n);
    s_emit_len += n;
    s_emit[s_emit_len++] = '\n';
    s_emit[s_emit_len] = '\0';
}

static void *t_alloc(size_t size) { return malloc(size); }
static void t_free(void *ptr) { free(ptr); }
static uint32_t t_dos(uint8_t index) { return ansi_cga_color(index); }
static uint8_t t_nearest(uint32_t rgb) { return ansi_cga_nearest(rgb); }

static const gfx3d_ops_t s_ops = {
    t_alloc, t_free, t_dos, t_nearest, t_emit, NULL
};

static void t_reset(void)
{
    s_emit[0] = '\0';
    s_emit_len = 0;
    memset(s_px, 0, sizeof(s_px));
    s_surface.px = s_px;
    s_surface.w = TEST_W;
    s_surface.h = TEST_H;
}

static void t_scene(gfx3d_ctx_t *ctx)
{
    gfx3d_viewport(ctx, 0, 0, TEST_W, TEST_H);
    gfx3d_camera(ctx,
                 gfx3d_vec3(0, 0, gfx3d_from_int(-8)),
                 gfx3d_vec3(0, 0, 0),
                 gfx3d_vec3(0, gfx3d_from_int(1), 0));
    gfx3d_projection(ctx, 90, 1, 1, gfx3d_from_int(1), gfx3d_from_int(100));
    gfx3d_light(ctx, gfx3d_vec3(0, 0, gfx3d_from_int(1)), 256);
    gfx3d_model_identity(ctx);
}

void test_engine3d_math_basics(void)
{
    t_reset();
    gfx3d_mat4_t id;
    gfx3d_mat4_t rot;
    gfx3d_vec3_t p;
    gfx3d_vec3_t q;

    TEST_ASSERT_EQUAL_INT(0, gfx3d_sin_deg(0));
    TEST_ASSERT_INT_WITHIN(64, GFX3D_ONE, gfx3d_sin_deg(90));
    TEST_ASSERT_INT_WITHIN(64, GFX3D_ONE, gfx3d_cos_deg(0));

    gfx3d_mat4_identity(&id);
    p = gfx3d_vec3(gfx3d_from_int(3), gfx3d_from_int(-2), gfx3d_from_int(5));
    q = gfx3d_mat4_transform_point(&id, p);
    TEST_ASSERT_EQUAL_INT(p.x, q.x);
    TEST_ASSERT_EQUAL_INT(p.y, q.y);
    TEST_ASSERT_EQUAL_INT(p.z, q.z);

    gfx3d_mat4_rotation_z(&rot, 90);
    q = gfx3d_mat4_transform_point(&rot,
            gfx3d_vec3(GFX3D_ONE, 0, 0));
    TEST_ASSERT_INT_WITHIN(GFX3D_ONE / 32, 0, q.x);
    TEST_ASSERT_INT_WITHIN(GFX3D_ONE / 32, GFX3D_ONE, q.y);
}

void test_engine3d_inverse_affine(void)
{
    t_reset();
    gfx3d_mat4_t t;
    gfx3d_mat4_t r;
    gfx3d_mat4_t inv;
    gfx3d_mat4_t back;
    gfx3d_vec3_t p;
    gfx3d_vec3_t q;

    gfx3d_mat4_translation(&t, gfx3d_from_int(2), gfx3d_from_int(3),
                           gfx3d_from_int(4));
    gfx3d_mat4_rotation_y(&r, 30);
    gfx3d_mat4_multiply(&t, &r, &t);
    TEST_ASSERT_EQUAL_INT(0, gfx3d_mat4_invert_affine(&inv, &t));
    gfx3d_mat4_multiply(&back, &inv, &t);
    p = gfx3d_vec3(gfx3d_from_int(7), gfx3d_from_int(-5), gfx3d_from_int(2));
    q = gfx3d_mat4_transform_point(&back, p);
    TEST_ASSERT_INT_WITHIN(GFX3D_ONE / 64, p.x, q.x);
    TEST_ASSERT_INT_WITHIN(GFX3D_ONE / 64, p.y, q.y);
    TEST_ASSERT_INT_WITHIN(GFX3D_ONE / 64, p.z, q.z);
}

void test_engine3d_init_and_shade_ramp(void)
{
    t_reset();
    gfx3d_ctx_t ctx;

    TEST_ASSERT_TRUE(gfx3d_init(&ctx, &s_ops));
    TEST_ASSERT_TRUE(ctx.ready);
    TEST_ASSERT_NOT_NULL(ctx.verts);
    TEST_ASSERT_NOT_NULL(ctx.tris);
    TEST_ASSERT_NOT_NULL(ctx.shades);
    TEST_ASSERT_EQUAL_UINT((uint32_t)(16 * GFX3D_SHADES * sizeof(uint16_t)),
                           ctx.stats.shade_bytes);
    TEST_ASSERT_EQUAL_UINT16(gfx_rgb_to_565(ansi_cga_color(12)),
                             ctx.shades[12 * GFX3D_SHADES + GFX3D_SHADES - 1]);
    gfx3d_close(&ctx);
    TEST_ASSERT_FALSE(ctx.ready);
    TEST_ASSERT_NULL(ctx.verts);
}

void test_engine3d_mesh_bounds(void)
{
    t_reset();
    gfx3d_ctx_t ctx;
    gfx3d_fixed v[9] = {0, 0, 0, GFX3D_ONE, 0, 0, 0, GFX3D_ONE, 0};
    uint16_t f[3] = {0, 1, 2};
    gfx3d_fixed bigv[3 * (GFX3D_MAX_MESH_VERTS + 1)];

    TEST_ASSERT_TRUE(gfx3d_init(&ctx, &s_ops));
    TEST_ASSERT_TRUE(gfx3d_mesh_create(&ctx, "tri", 12, 3, v, 1, f));
    TEST_ASSERT_NOT_NULL(gfx3d_mesh_find(&ctx, "tri"));
    TEST_ASSERT_EQUAL_INT(3, gfx3d_mesh_find(&ctx, "tri")->nv);
    TEST_ASSERT_EQUAL_INT(0, ctx.stats.overflow);

    memset(bigv, 0, sizeof(bigv));
    TEST_ASSERT_FALSE(gfx3d_mesh_create(&ctx, "big", 9,
                                        GFX3D_MAX_MESH_VERTS + 1, bigv, 1, f));
    TEST_ASSERT_EQUAL_INT(1, ctx.stats.overflow);
    TEST_ASSERT_NULL(gfx3d_mesh_find(&ctx, "big"));
    gfx3d_close(&ctx);
}

void test_engine3d_backface_cull(void)
{
    t_reset();
    gfx3d_ctx_t ctx;
    /* A front-facing triangle (+z normal) and the same triangle reversed. */
    gfx3d_fixed v[9] = {
        gfx3d_from_int(-2), gfx3d_from_int(-2), 0,
        gfx3d_from_int(2),  gfx3d_from_int(-2), 0,
        0,                  gfx3d_from_int(2),  0,
    };
    uint16_t f[6] = {0, 1, 2, 0, 2, 1};

    TEST_ASSERT_TRUE(gfx3d_init(&ctx, &s_ops));
    t_scene(&ctx);
    gfx3d_mode(&ctx, GFX3D_SOLID, 1);
    TEST_ASSERT_TRUE(gfx3d_mesh_create(&ctx, "pair", 12, 3, v, 2, f));
    gfx3d_render(&ctx, &s_surface, gfx3d_mesh_find(&ctx, "pair"));

    TEST_ASSERT_EQUAL_INT(2, ctx.stats.tris);
    TEST_ASSERT_EQUAL_INT(1, ctx.stats.culled);
    TEST_ASSERT_EQUAL_INT(1, ctx.stats.drawn);
    TEST_ASSERT_EQUAL_INT(0, ctx.stats.overflow);
    TEST_ASSERT_NOT_NULL(strstr(s_emit, "e3d.cull id=1"));
    TEST_ASSERT_EQUAL_UINT16(gfx_rgb_to_565(ansi_cga_color(12)), s_px[16 * TEST_W + 16]);
    gfx3d_close(&ctx);
}

void test_engine3d_depth_order(void)
{
    t_reset();
    gfx3d_ctx_t ctx;
    /* Triangle 0 is near (z=-4, base 12); triangle 1 is far (z=0, base 9). */
    gfx3d_fixed v[18] = {
        gfx3d_from_int(-2), gfx3d_from_int(-2), gfx3d_from_int(-4),
        gfx3d_from_int(2),  gfx3d_from_int(-2), gfx3d_from_int(-4),
        0,                  gfx3d_from_int(2),  gfx3d_from_int(-4),
        gfx3d_from_int(-2), gfx3d_from_int(-2), 0,
        gfx3d_from_int(2),  gfx3d_from_int(-2), 0,
        0,                  gfx3d_from_int(2),  0,
    };
    uint16_t f[6] = {0, 1, 2, 3, 4, 5};
    char *near_draw;
    char *far_draw;

    TEST_ASSERT_TRUE(gfx3d_init(&ctx, &s_ops));
    t_scene(&ctx);
    gfx3d_mode(&ctx, GFX3D_SOLID, 1);
    TEST_ASSERT_TRUE(gfx3d_mesh_create(&ctx, "depth", 12, 6, v, 2, f));
    gfx3d_render(&ctx, &s_surface, gfx3d_mesh_find(&ctx, "depth"));

    TEST_ASSERT_EQUAL_INT(2, ctx.stats.drawn);
    far_draw = strstr(s_emit, "e3d.draw id=1");
    near_draw = strstr(s_emit, "e3d.draw id=0");
    TEST_ASSERT_NOT_NULL(far_draw);
    TEST_ASSERT_NOT_NULL(near_draw);
    TEST_ASSERT_TRUE(far_draw < near_draw);
    TEST_ASSERT_EQUAL_UINT16(gfx_rgb_to_565(ansi_cga_color(12)),
                             s_px[16 * TEST_W + 16]);
    gfx3d_close(&ctx);
}

void test_engine3d_wire_and_clip(void)
{
    t_reset();
    gfx3d_ctx_t ctx;
    gfx3d_fixed behind[9] = {
        gfx3d_from_int(-2), gfx3d_from_int(-2), gfx3d_from_int(-20),
        gfx3d_from_int(2),  gfx3d_from_int(-2), gfx3d_from_int(-20),
        0,                  gfx3d_from_int(2),  gfx3d_from_int(-20),
    };
    gfx3d_fixed visible[9] = {
        gfx3d_from_int(-2), gfx3d_from_int(-2), 0,
        gfx3d_from_int(2),  gfx3d_from_int(-2), 0,
        0,                  gfx3d_from_int(2),  0,
    };
    uint16_t f[3] = {0, 1, 2};
    int ink = 0;
    int i;

    TEST_ASSERT_TRUE(gfx3d_init(&ctx, &s_ops));
    t_scene(&ctx);
    gfx3d_mode(&ctx, GFX3D_WIRE, 1);

    TEST_ASSERT_TRUE(gfx3d_mesh_create(&ctx, "behind", 9, 3, behind, 1, f));
    gfx3d_render(&ctx, &s_surface, gfx3d_mesh_find(&ctx, "behind"));
    TEST_ASSERT_EQUAL_INT(0, ctx.stats.drawn);
    TEST_ASSERT_TRUE(ctx.stats.clipped >= 1);
    TEST_ASSERT_EQUAL_INT(0, ctx.stats.overflow);

    t_reset();
    TEST_ASSERT_TRUE(gfx3d_mesh_create(&ctx, "vis", 15, 3, visible, 1, f));
    gfx3d_render(&ctx, &s_surface, gfx3d_mesh_find(&ctx, "vis"));
    TEST_ASSERT_EQUAL_INT(1, ctx.stats.drawn);
    for (i = 0; i < TEST_W * TEST_H; i++) {
        if (s_px[i] != 0) ink++;
    }
    TEST_ASSERT_TRUE(ink > 0);
    gfx3d_close(&ctx);
}
