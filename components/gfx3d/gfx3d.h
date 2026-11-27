/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file gfx3d.h
 * @module gfx3d
 * @layer leaf
 * @domain ui
 * @provides fixed-point 4x4 math, camera/projection, cull/clip/sort/shade, one-path render
 * @status stable
 * @test test/main/test_engine3d.c
 * @hard-rule ai-context.md#hard-rules
 * @brief Minimal fixed-point 3D engine for P4MiniShell batch games.
 *
 * Design note: the matrix/vector algorithms follow jthwho/bmds
 * `sdk/3d/math.c` (MIT, (c) Jason Howard, pinned commit ef05be9f), re-expressed
 * here in Q16.16 fixed point with an integer sine table; the shade-ramp idea
 * follows that project's `sdk/3d/light.c`. Both are credited in the .c files.
 * No BMDS raster, canvas, mesh, model or GUI code is used: all pixels are
 * drawn by components/gfx through gfx_surface_triangle()/gfx_surface_line()
 * and the one DOS/CGA palette (ansi_cga_color/ansi_cga_nearest), supplied to
 * the core through function pointers so this translation unit stays pure and
 * headless-safe.
 *
 * The engine owns no display surface: the caller hands it a gfx_surface_t
 * (the single `gfx` canvas). Bulk buffers (scratch vertices, triangle queue,
 * shade ramp, mesh vertex/face lists) come from the injected allocator, which
 * the firmware binds to the PSRAM-first p4heap and the host gate binds to
 * malloc. No large array is ever a stack local here (32 KiB stack rule).
 */
#ifndef P4MINISHELL_GFX3D_H
#define P4MINISHELL_GFX3D_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------
 * Fixed-point representation
 * ------------------------------------------------------------------------ */

/** Q16.16 fixed point. All transforms, normals and shading use this type. */
typedef int32_t gfx3d_fixed;

#define GFX3D_FP_SHIFT 16
#define GFX3D_ONE      (INT32_C(1) << GFX3D_FP_SHIFT)
#define GFX3D_HALF     (GFX3D_ONE / 2)

/** Multiply/divide two Q16.16 values with a 64-bit intermediate. */
static inline gfx3d_fixed gfx3d_fp_mul(gfx3d_fixed a, gfx3d_fixed b)
{
    return (gfx3d_fixed)(((int64_t)a * (int64_t)b) >> GFX3D_FP_SHIFT);
}

static inline gfx3d_fixed gfx3d_fp_div(gfx3d_fixed a, gfx3d_fixed b)
{
    if (b == 0) return 0;
    return (gfx3d_fixed)(((int64_t)a << GFX3D_FP_SHIFT) / (int64_t)b);
}

static inline gfx3d_fixed gfx3d_from_int(int v)
{
    return (gfx3d_fixed)((int64_t)v * GFX3D_ONE);
}

static inline int gfx3d_to_int(gfx3d_fixed v)
{
    return (int)(v >= 0 ? (v + GFX3D_HALF) >> GFX3D_FP_SHIFT
                        : -((-v + GFX3D_HALF) >> GFX3D_FP_SHIFT));
}

/* ------------------------------------------------------------------------
 * Bounded capacities (compile-time; bulk storage is allocated at init)
 * ------------------------------------------------------------------------ */

#define GFX3D_MAX_VERTS   512   /**< Per-frame scratch vertex buffer. */
#define GFX3D_MAX_TRIS    256   /**< Per-frame depth-sorted triangle queue. */
#define GFX3D_MAX_MESHES  8     /**< Named numeric meshes. */
#define GFX3D_MAX_MESH_VERTS 128 /**< Vertices per mesh. */
#define GFX3D_MAX_MESH_FACES 256 /**< Faces per mesh. */
#define GFX3D_MAX_CLIP_VERTS 8  /**< Near-plane clip polygon scratch. */
#define GFX3D_SHADES      8     /**< DOS shade-ramp levels per base colour. */
#define GFX3D_NAME_MAX    16

#define GFX3D_WIRE  0
#define GFX3D_SOLID 1

/* Out-code bits: a vertex outside a clip/screen plane. */
#define GFX3D_OUT_NEAR   0x01u
#define GFX3D_OUT_FAR    0x02u
#define GFX3D_OUT_LEFT   0x04u
#define GFX3D_OUT_RIGHT  0x08u
#define GFX3D_OUT_TOP    0x10u
#define GFX3D_OUT_BOTTOM 0x20u
#define GFX3D_OUT_DEPTH  (GFX3D_OUT_NEAR | GFX3D_OUT_FAR)

/* ------------------------------------------------------------------------
 * Vectors and matrices (column-major, m[column * 4 + row], as GL/BMDS)
 * ------------------------------------------------------------------------ */

typedef struct {
    gfx3d_fixed x;
    gfx3d_fixed y;
    gfx3d_fixed z;
} gfx3d_vec3_t;

typedef struct {
    gfx3d_fixed x;
    gfx3d_fixed y;
    gfx3d_fixed z;
    gfx3d_fixed w;
} gfx3d_vec4_t;

typedef struct {
    gfx3d_fixed m[16];
} gfx3d_mat4_t;

/** Headers of the pure math functions (implemented in gfx3d_math.c). */
gfx3d_fixed     gfx3d_sin_deg(int degrees);
gfx3d_fixed     gfx3d_cos_deg(int degrees);
gfx3d_fixed     gfx3d_isqrt_q16(gfx3d_fixed x);
gfx3d_vec3_t    gfx3d_vec3(gfx3d_fixed x, gfx3d_fixed y, gfx3d_fixed z);
gfx3d_vec3_t    gfx3d_vec3_add(gfx3d_vec3_t a, gfx3d_vec3_t b);
gfx3d_vec3_t    gfx3d_vec3_sub(gfx3d_vec3_t a, gfx3d_vec3_t b);
gfx3d_vec3_t    gfx3d_vec3_scale(gfx3d_vec3_t a, gfx3d_fixed k);
gfx3d_fixed     gfx3d_vec3_dot(gfx3d_vec3_t a, gfx3d_vec3_t b);
gfx3d_vec3_t    gfx3d_vec3_cross(gfx3d_vec3_t a, gfx3d_vec3_t b);
gfx3d_fixed     gfx3d_vec3_length(gfx3d_vec3_t a);
gfx3d_vec3_t    gfx3d_vec3_normalize(gfx3d_vec3_t a);
void            gfx3d_mat4_identity(gfx3d_mat4_t *m);
void            gfx3d_mat4_multiply(gfx3d_mat4_t *out, const gfx3d_mat4_t *a,
                                    const gfx3d_mat4_t *b);
void            gfx3d_mat4_translation(gfx3d_mat4_t *m, gfx3d_fixed x,
                                       gfx3d_fixed y, gfx3d_fixed z);
void            gfx3d_mat4_scaling(gfx3d_mat4_t *m, gfx3d_fixed x,
                                   gfx3d_fixed y, gfx3d_fixed z);
void            gfx3d_mat4_rotation_x(gfx3d_mat4_t *m, int degrees);
void            gfx3d_mat4_rotation_y(gfx3d_mat4_t *m, int degrees);
void            gfx3d_mat4_rotation_z(gfx3d_mat4_t *m, int degrees);
void            gfx3d_mat4_rotation_axis(gfx3d_mat4_t *m, gfx3d_vec3_t axis,
                                         int degrees);
void            gfx3d_mat4_look_at(gfx3d_mat4_t *m, gfx3d_vec3_t eye,
                                   gfx3d_vec3_t target, gfx3d_vec3_t up);
void            gfx3d_mat4_perspective(gfx3d_mat4_t *m, int fovy_deg,
                                       int aspect_num, int aspect_den,
                                       gfx3d_fixed near_z, gfx3d_fixed far_z);
void            gfx3d_mat4_ortho(gfx3d_mat4_t *m, gfx3d_fixed left,
                                 gfx3d_fixed right, gfx3d_fixed bottom,
                                 gfx3d_fixed top, gfx3d_fixed near_z,
                                 gfx3d_fixed far_z);
int             gfx3d_mat4_invert_affine(gfx3d_mat4_t *out,
                                         const gfx3d_mat4_t *m);
gfx3d_vec4_t    gfx3d_mat4_transform(const gfx3d_mat4_t *m, gfx3d_vec3_t p);
gfx3d_vec3_t    gfx3d_mat4_transform_point(const gfx3d_mat4_t *m,
                                           gfx3d_vec3_t p);
gfx3d_vec3_t    gfx3d_mat4_transform_vector(const gfx3d_mat4_t *m,
                                            gfx3d_vec3_t v);

/* ------------------------------------------------------------------------
 * Engine objects
 * ------------------------------------------------------------------------ */

/** A transformed vertex: clip space, view space, screen position, shade. */
typedef struct {
    gfx3d_vec4_t clip;
    gfx3d_vec3_t view;
    gfx3d_fixed  shade;   /**< 0..GFX3D_ONE (flat: face intensity). */
    int          sx;
    int          sy;
    gfx3d_fixed  depth;   /**< View-space distance; larger is farther. */
    uint8_t      out;     /**< GFX3D_OUT_* out-code. */
    uint8_t      reserved;
} gfx3d_vertex_t;

/** A queued, screen-space triangle after cull/clip/sort. */
typedef struct {
    gfx3d_vertex_t v[3];
    gfx3d_fixed    depth; /**< Sort key: farthest first (painter's order). */
    uint16_t       color; /**< RGB565 from the one DOS ramp. */
    uint16_t       id;    /**< Deterministic id (tie-break in the sort). */
} gfx3d_tri_t;

/** A named numeric mesh; vertex/face lists are PSRAM buffers. */
typedef struct {
    char          name[GFX3D_NAME_MAX];
    uint8_t       dos_color;
    int           nv;
    int           nf;
    gfx3d_vec3_t *v;      /**< nv vertices. */
    uint16_t     *f;      /**< nf * 3 indices. */
    uint8_t      *face_colors; /**< Optional per-face DOS index (NULL = uniform). */
} gfx3d_mesh_t;

/** Runtime counters; all bounded by the capacities above. */
typedef struct {
    int verts;        /**< Vertices transformed this frame. */
    int tris;         /**< Faces submitted this frame. */
    int clipped;      /**< Faces removed by near/screen clipping. */
    int culled;       /**< Back faces removed. */
    int drawn;        /**< Raster calls actually issued. */
    int overflow;     /**< Requests refused by a bounded buffer. */
    gfx3d_fixed last_depth; /**< Depth of the last drawn triangle. */
    uint32_t shade_bytes;   /**< Bytes in the shade ramp. */
} gfx3d_stats_t;

/** Injected services: no ESP headers in this translation unit. */
typedef struct {
    void        *(*alloc)(size_t size);          /**< Bulk (PSRAM-first). */
    void         (*release)(void *ptr);          /**< Null-safe free. */
    uint32_t     (*dos_color)(uint8_t index);    /**< The one DOS/RGB888 palette. */
    uint8_t      (*dos_nearest)(uint32_t rgb);   /**< The one nearest mapper. */
    void         (*emit)(const char *line, void *ud); /**< Optional trace sink. */
    void         *emit_ud;
} gfx3d_ops_t;

/** The engine context. Allocate it from the caller's pool, never on a stack. */
typedef struct {
    gfx3d_ops_t    ops;
    gfx3d_mat4_t   model;
    gfx3d_mat4_t   view;
    gfx3d_mat4_t   proj;
    gfx3d_vec3_t   light_dir;  /**< Unit direction toward the light (view space). */
    gfx3d_fixed    ambient;    /**< 0..GFX3D_ONE. */
    int            fovy_deg;
    int            aspect_num;
    int            aspect_den;
    gfx3d_fixed    near_z;
    gfx3d_fixed    far_z;
    int            vp_x;
    int            vp_y;
    int            vp_w;
    int            vp_h;
    int            mode;       /**< GFX3D_WIRE or GFX3D_SOLID. */
    int            cull;       /**< 0 = no cull, 1 = backface cull. */

    gfx3d_vertex_t *verts;     /**< GFX3D_MAX_VERTS scratch (PSRAM). */
    gfx3d_tri_t    *tris;      /**< GFX3D_MAX_TRIS queue (PSRAM). */
    uint16_t       *shades;    /**< 16 * GFX3D_SHADES RGB565 ramp (PSRAM). */
    gfx3d_mesh_t    meshes[GFX3D_MAX_MESHES]; /* small control blocks */
    gfx3d_stats_t   stats;
    bool            ready;
} gfx3d_ctx_t;

/* ------------------------------------------------------------------------
 * Lifecycle and scene setup
 * ------------------------------------------------------------------------ */

/** Bind services and allocate bounded buffers; idempotent-safe on a fresh ctx. */
bool gfx3d_init(gfx3d_ctx_t *ctx, const gfx3d_ops_t *ops);

/** Free every engine-owned buffer and reset state (NULL-safe). */
void gfx3d_close(gfx3d_ctx_t *ctx);

/** Set the raster viewport in surface pixels (clipped to the surface at draw). */
void gfx3d_viewport(gfx3d_ctx_t *ctx, int x, int y, int w, int h);

/** Set the view matrix from eye/target/up (Q16.16 world coordinates). */
void gfx3d_camera(gfx3d_ctx_t *ctx, gfx3d_vec3_t eye, gfx3d_vec3_t target,
                  gfx3d_vec3_t up);

/** Set a perspective projection (integer degrees/aspect, fixed near/far). */
void gfx3d_projection(gfx3d_ctx_t *ctx, int fovy_deg, int aspect_num,
                      int aspect_den, gfx3d_fixed near_z, gfx3d_fixed far_z);

/** Reset the model matrix to identity. */
void gfx3d_model_identity(gfx3d_ctx_t *ctx);

/** Post-multiply the model matrix by a translation. */
void gfx3d_model_translate(gfx3d_ctx_t *ctx, gfx3d_fixed x, gfx3d_fixed y,
                           gfx3d_fixed z);

/** Post-multiply the model matrix by a scale. */
void gfx3d_model_scale(gfx3d_ctx_t *ctx, gfx3d_fixed x, gfx3d_fixed y,
                       gfx3d_fixed z);

/** Post-multiply the model matrix by a rotation about a principal axis. */
void gfx3d_model_rotate(gfx3d_ctx_t *ctx, char axis, int degrees);

/** Set render mode and backface-cull flag. */
void gfx3d_mode(gfx3d_ctx_t *ctx, int mode, int cull);

/** Set the directional light (integer world direction) and ambient 0..256. */
void gfx3d_light(gfx3d_ctx_t *ctx, gfx3d_vec3_t direction, int ambient_256);

/* ------------------------------------------------------------------------
 * Meshes and rendering
 * ------------------------------------------------------------------------ */

/**
 * Create/replace a named mesh from numeric vertex and face lists.
 * @param verts 3*nv Q16.16 coordinates (x,y,z triples).
 * @param faces 3*nf vertex indices.
 * @return false when a bound is exceeded or allocation fails (no partial mesh).
 */
bool gfx3d_mesh_create(gfx3d_ctx_t *ctx, const char *name, uint8_t dos_color,
                       int nv, const gfx3d_fixed *verts,
                       int nf, const uint16_t *faces);

/** Find a named mesh, or NULL. */
gfx3d_mesh_t *gfx3d_mesh_find(gfx3d_ctx_t *ctx, const char *name);

/** Optional per-face DOS base colour (lazy PSRAM array, one byte per face).
 *  Faces without an override keep the mesh's uniform colour. */
bool gfx3d_mesh_face_color(gfx3d_ctx_t *ctx, gfx3d_mesh_t *mesh, int face,
                           uint8_t dos_color);

/** Free all mesh buffers. */
void gfx3d_mesh_clear_all(gfx3d_ctx_t *ctx);

/** Clear the ONE gfx surface (delegates to gfx_surface_clear). */
void gfx3d_clear(gfx_surface_t *surface, uint16_t color);

/**
 * Transform, cull, clip, depth-sort and draw one mesh into @p surface.
 * Every pixel is written by gfx_surface_triangle()/gfx_surface_line():
 * there is no second rasterizer. Optional emit() lines report each
 * submitted/culled/drawn triangle for the host proof gate.
 */
void gfx3d_render(gfx3d_ctx_t *ctx, gfx_surface_t *surface,
                  const gfx3d_mesh_t *mesh);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_GFX3D_H */
