/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file gfx3d_core.c
 * @brief Camera/projection, cull/clip/sort/shade and the single-path renderer.
 *
 * The DOS shade ramp follows the shade-table idea in jthwho/bmds
 * `sdk/3d/light.c` (MIT, (c) Jason Howard, pinned commit ef05be9f,
 * tools/LICENSE-BMDS-MIT.txt): interpolate a base palette entry toward dark
 * and choose the nearest palette entry at each level. Here the lookup goes
 * through the one ansi_cga_nearest() service injected by the caller, so there
 * is no second palette or second nearest-colour implementation.
 *
 * Every visible triangle is painted with gfx_surface_triangle()/line(): the
 * engine never touches a pixel buffer directly and never forks a rasterizer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx3d.h"

/* Optional diagnostic sink. A NULL sink is a supported "silent" mode, as in
 * the other headless cores: nothing here fakes a hardware result. */
static void emit_line(gfx3d_ctx_t *ctx, const char *line)
{
    if (ctx != NULL && ctx->ops.emit != NULL) {
        ctx->ops.emit(line, ctx->ops.emit_ud);
    }
}

void gfx3d_clear(gfx_surface_t *surface, uint16_t color)
{
    gfx_surface_clear(surface, color);
}

/* ------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------ */

bool gfx3d_init(gfx3d_ctx_t *ctx, const gfx3d_ops_t *ops)
{
    int i;

    if (ctx == NULL || ops == NULL || ops->alloc == NULL || ops->release == NULL ||
        ops->dos_color == NULL || ops->dos_nearest == NULL) {
        return false;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->ops = *ops;
    ctx->verts = (gfx3d_vertex_t *)ops->alloc(
        (size_t)GFX3D_MAX_VERTS * sizeof(*ctx->verts));
    ctx->tris = (gfx3d_tri_t *)ops->alloc(
        (size_t)GFX3D_MAX_TRIS * sizeof(*ctx->tris));
    ctx->shades = (uint16_t *)ops->alloc(
        (size_t)16 * GFX3D_SHADES * sizeof(*ctx->shades));
    if (ctx->verts == NULL || ctx->tris == NULL || ctx->shades == NULL) {
        gfx3d_close(ctx);
        return false;
    }
    ctx->stats.shade_bytes = (uint32_t)(16 * GFX3D_SHADES * sizeof(uint16_t));

    gfx3d_mat4_identity(&ctx->model);
    gfx3d_mat4_identity(&ctx->view);
    gfx3d_mat4_identity(&ctx->proj);
    ctx->light_dir = gfx3d_vec3(0, 0, GFX3D_ONE);
    ctx->ambient = GFX3D_ONE / 4;
    ctx->fovy_deg = 90;
    ctx->aspect_num = 1;
    ctx->aspect_den = 1;
    ctx->near_z = GFX3D_ONE;
    ctx->far_z = gfx3d_from_int(100);
    ctx->vp_w = 64;
    ctx->vp_h = 64;
    ctx->mode = GFX3D_SOLID;
    ctx->cull = 1;

    /* Shade ramp: base DOS index -> 8 levels, darkest first. Each level is
     * the nearest DOS entry to the lerped colour, through the one palette. */
    for (i = 0; i < 16; i++) {
        uint32_t base = ops->dos_color((uint8_t)i);
        int level;

        for (level = 0; level < GFX3D_SHADES; level++) {
            int factor = level + 1;
            int r = (int)((base >> 16) & 0xFFu) * factor / GFX3D_SHADES;
            int g = (int)((base >> 8) & 0xFFu) * factor / GFX3D_SHADES;
            int b = (int)(base & 0xFFu) * factor / GFX3D_SHADES;
            uint8_t idx = ops->dos_nearest((uint32_t)(r << 16 | g << 8 | b));

            ctx->shades[i * GFX3D_SHADES + level] =
                gfx_rgb_to_565(ops->dos_color(idx));
        }
    }
    ctx->ready = true;
    return true;
}

void gfx3d_close(gfx3d_ctx_t *ctx)
{
    if (ctx == NULL) return;
    gfx3d_mesh_clear_all(ctx);
    if (ctx->verts != NULL) ctx->ops.release(ctx->verts);
    if (ctx->tris != NULL) ctx->ops.release(ctx->tris);
    if (ctx->shades != NULL) ctx->ops.release(ctx->shades);
    memset(ctx, 0, sizeof(*ctx));
}

/* ------------------------------------------------------------------------
 * Scene setup
 * ------------------------------------------------------------------------ */

void gfx3d_viewport(gfx3d_ctx_t *ctx, int x, int y, int w, int h)
{
    if (ctx == NULL) return;
    ctx->vp_x = x;
    ctx->vp_y = y;
    ctx->vp_w = w < 1 ? 1 : w;
    ctx->vp_h = h < 1 ? 1 : h;
}

void gfx3d_camera(gfx3d_ctx_t *ctx, gfx3d_vec3_t eye, gfx3d_vec3_t target,
                  gfx3d_vec3_t up)
{
    if (ctx == NULL) return;
    gfx3d_mat4_look_at(&ctx->view, eye, target, up);
}

void gfx3d_projection(gfx3d_ctx_t *ctx, int fovy_deg, int aspect_num,
                      int aspect_den, gfx3d_fixed near_z, gfx3d_fixed far_z)
{
    if (ctx == NULL) return;
    ctx->fovy_deg = fovy_deg;
    ctx->aspect_num = aspect_num < 1 ? 1 : aspect_num;
    ctx->aspect_den = aspect_den < 1 ? 1 : aspect_den;
    ctx->near_z = near_z > 0 ? near_z : GFX3D_ONE;
    ctx->far_z = far_z > ctx->near_z ? far_z : ctx->near_z + GFX3D_ONE;
    gfx3d_mat4_perspective(&ctx->proj, ctx->fovy_deg, ctx->aspect_num,
                           ctx->aspect_den, ctx->near_z, ctx->far_z);
}

void gfx3d_model_identity(gfx3d_ctx_t *ctx)
{
    if (ctx != NULL) gfx3d_mat4_identity(&ctx->model);
}

void gfx3d_model_translate(gfx3d_ctx_t *ctx, gfx3d_fixed x, gfx3d_fixed y,
                           gfx3d_fixed z)
{
    gfx3d_mat4_t t;
    gfx3d_mat4_t out;

    if (ctx == NULL) return;
    gfx3d_mat4_translation(&t, x, y, z);
    gfx3d_mat4_multiply(&out, &ctx->model, &t);
    ctx->model = out;
}

void gfx3d_model_scale(gfx3d_ctx_t *ctx, gfx3d_fixed x, gfx3d_fixed y,
                       gfx3d_fixed z)
{
    gfx3d_mat4_t t;
    gfx3d_mat4_t out;

    if (ctx == NULL) return;
    gfx3d_mat4_scaling(&t, x, y, z);
    gfx3d_mat4_multiply(&out, &ctx->model, &t);
    ctx->model = out;
}

void gfx3d_model_rotate(gfx3d_ctx_t *ctx, char axis, int degrees)
{
    gfx3d_mat4_t t;
    gfx3d_mat4_t out;

    if (ctx == NULL) return;
    switch (axis) {
    case 'x': case 'X': gfx3d_mat4_rotation_x(&t, degrees); break;
    case 'y': case 'Y': gfx3d_mat4_rotation_y(&t, degrees); break;
    case 'z': case 'Z': gfx3d_mat4_rotation_z(&t, degrees); break;
    default: return;
    }
    gfx3d_mat4_multiply(&out, &ctx->model, &t);
    ctx->model = out;
}

void gfx3d_mode(gfx3d_ctx_t *ctx, int mode, int cull)
{
    if (ctx == NULL) return;
    ctx->mode = (mode == GFX3D_WIRE) ? GFX3D_WIRE : GFX3D_SOLID;
    ctx->cull = cull ? 1 : 0;
}

void gfx3d_light(gfx3d_ctx_t *ctx, gfx3d_vec3_t direction, int ambient_256)
{
    if (ctx == NULL) return;
    ctx->light_dir = gfx3d_vec3_normalize(direction);
    if (ctx->light_dir.x == 0 && ctx->light_dir.y == 0 && ctx->light_dir.z == 0) {
        ctx->light_dir = gfx3d_vec3(0, 0, GFX3D_ONE);
    }
    if (ambient_256 < 0) ambient_256 = 0;
    if (ambient_256 > 256) ambient_256 = 256;
    ctx->ambient = (gfx3d_fixed)((int64_t)ambient_256 * GFX3D_ONE / 256);
}

/* ------------------------------------------------------------------------
 * Numeric meshes
 * ------------------------------------------------------------------------ */

bool gfx3d_mesh_create(gfx3d_ctx_t *ctx, const char *name, uint8_t dos_color,
                       int nv, const gfx3d_fixed *verts,
                       int nf, const uint16_t *faces)
{
    gfx3d_mesh_t *dst = NULL;
    gfx3d_vec3_t *vbuf = NULL;
    uint16_t *fbuf = NULL;
    int i;

    if (ctx == NULL || name == NULL || verts == NULL || faces == NULL) return false;
    if (nv < 3 || nv > GFX3D_MAX_MESH_VERTS || nf < 1 ||
        nf > GFX3D_MAX_MESH_FACES) {
        ctx->stats.overflow++;
        return false;
    }
    for (i = 0; i < GFX3D_MAX_MESHES; i++) {
        if (ctx->meshes[i].name[0] == '\0' || strcmp(ctx->meshes[i].name, name) == 0) {
            dst = &ctx->meshes[i];
            break;
        }
    }
    if (dst == NULL) {
        ctx->stats.overflow++;
        return false;
    }
    vbuf = (gfx3d_vec3_t *)ctx->ops.alloc((size_t)nv * sizeof(*vbuf));
    fbuf = (uint16_t *)ctx->ops.alloc((size_t)nf * 3 * sizeof(*fbuf));
    if (vbuf == NULL || fbuf == NULL) {
        if (vbuf != NULL) ctx->ops.release(vbuf);
        if (fbuf != NULL) ctx->ops.release(fbuf);
        ctx->stats.overflow++;
        return false;
    }
    for (i = 0; i < nv; i++) {
        vbuf[i] = gfx3d_vec3(verts[i * 3], verts[i * 3 + 1], verts[i * 3 + 2]);
    }
    memcpy(fbuf, faces, (size_t)nf * 3 * sizeof(*fbuf));
    /* Replace only after both buffers exist: a failed create never leaves a
     * half-written mesh behind. */
    if (dst->v != NULL) ctx->ops.release(dst->v);
    if (dst->f != NULL) ctx->ops.release(dst->f);
    if (dst->face_colors != NULL) ctx->ops.release(dst->face_colors);
    memset(dst, 0, sizeof(*dst));
    strncpy(dst->name, name, GFX3D_NAME_MAX - 1);
    dst->dos_color = dos_color & 0x0F;
    dst->nv = nv;
    dst->nf = nf;
    dst->v = vbuf;
    dst->f = fbuf;
    return true;
}

bool gfx3d_mesh_face_color(gfx3d_ctx_t *ctx, gfx3d_mesh_t *mesh, int face,
                            uint8_t dos_color)
{
    if (ctx == NULL || mesh == NULL || mesh->f == NULL || face < 0 ||
        face >= mesh->nf) {
        return false;
    }
    if (mesh->face_colors == NULL) {
        mesh->face_colors = (uint8_t *)ctx->ops.alloc(
            (size_t)GFX3D_MAX_MESH_FACES * sizeof(uint8_t));
        if (mesh->face_colors == NULL) return false;
        memset(mesh->face_colors, mesh->dos_color,
               (size_t)GFX3D_MAX_MESH_FACES * sizeof(uint8_t));
    }
    mesh->face_colors[face] = dos_color & 0x0F;
    return true;
}

gfx3d_mesh_t *gfx3d_mesh_find(gfx3d_ctx_t *ctx, const char *name)
{
    int i;

    if (ctx == NULL || name == NULL) return NULL;
    for (i = 0; i < GFX3D_MAX_MESHES; i++) {
        if (ctx->meshes[i].name[0] != '\0' &&
            strcmp(ctx->meshes[i].name, name) == 0) {
            return &ctx->meshes[i];
        }
    }
    return NULL;
}

void gfx3d_mesh_clear_all(gfx3d_ctx_t *ctx)
{
    int i;

    if (ctx == NULL) return;
    for (i = 0; i < GFX3D_MAX_MESHES; i++) {
        if (ctx->meshes[i].v != NULL) ctx->ops.release(ctx->meshes[i].v);
        if (ctx->meshes[i].f != NULL) ctx->ops.release(ctx->meshes[i].f);
        if (ctx->meshes[i].face_colors != NULL) {
            ctx->ops.release(ctx->meshes[i].face_colors);
        }
        memset(&ctx->meshes[i], 0, sizeof(ctx->meshes[i]));
    }
}

/* ------------------------------------------------------------------------
 * Transform, clip and sort
 * ------------------------------------------------------------------------ */

/** Linear interpolation of a whole transformed vertex at t in [0,1] Q16.16. */
static gfx3d_vertex_t vertex_lerp(const gfx3d_vertex_t *a,
                                  const gfx3d_vertex_t *b, gfx3d_fixed t)
{
    gfx3d_vertex_t r;

    memset(&r, 0, sizeof(r));
    r.clip.x = a->clip.x + gfx3d_fp_mul(b->clip.x - a->clip.x, t);
    r.clip.y = a->clip.y + gfx3d_fp_mul(b->clip.y - a->clip.y, t);
    r.clip.z = a->clip.z + gfx3d_fp_mul(b->clip.z - a->clip.z, t);
    r.clip.w = a->clip.w + gfx3d_fp_mul(b->clip.w - a->clip.w, t);
    r.view = gfx3d_vec3(
        a->view.x + gfx3d_fp_mul(b->view.x - a->view.x, t),
        a->view.y + gfx3d_fp_mul(b->view.y - a->view.y, t),
        a->view.z + gfx3d_fp_mul(b->view.z - a->view.z, t));
    r.shade = a->shade + gfx3d_fp_mul(b->shade - a->shade, t);
    return r;
}

/** Transform one mesh vertex into clip/view space (no projection yet). */
static gfx3d_vertex_t transform_vertex(gfx3d_ctx_t *ctx, gfx3d_vec3_t p)
{
    gfx3d_mat4_t mvp;
    gfx3d_vertex_t r;
    gfx3d_vec3_t world = gfx3d_mat4_transform_point(&ctx->model, p);
    gfx3d_fixed d;

    gfx3d_mat4_multiply(&mvp, &ctx->proj, &ctx->view);
    gfx3d_mat4_multiply(&mvp, &mvp, &ctx->model);
    memset(&r, 0, sizeof(r));
    r.clip = gfx3d_mat4_transform(&mvp, p);
    r.view = gfx3d_mat4_transform_point(&ctx->view, world);
    d = -r.view.z;
    r.depth = d > 0 ? d : 0;
    return r;
}

/** Project a transformed vertex to integer screen coordinates and out-code. */
static void project_vertex(gfx3d_ctx_t *ctx, gfx3d_vertex_t *v)
{
    gfx3d_fixed nx;
    gfx3d_fixed ny;
    gfx3d_fixed half_w = gfx3d_from_int(ctx->vp_w) / 2;
    gfx3d_fixed half_h = gfx3d_from_int(ctx->vp_h) / 2;

    v->out = 0;
    if (v->clip.w <= ctx->near_z) {
        v->out |= GFX3D_OUT_NEAR;
    }
    if (v->depth > ctx->far_z) {
        v->out |= GFX3D_OUT_FAR;
    }
    if (v->out & GFX3D_OUT_NEAR) {
        v->sx = ctx->vp_x + ctx->vp_w / 2;
        v->sy = ctx->vp_y + ctx->vp_h / 2;
        return;
    }
    nx = gfx3d_fp_div(v->clip.x, v->clip.w);
    ny = gfx3d_fp_div(v->clip.y, v->clip.w);
    v->sx = ctx->vp_x + gfx3d_to_int(gfx3d_fp_mul(nx + GFX3D_ONE, half_w));
    v->sy = ctx->vp_y + gfx3d_to_int(gfx3d_fp_mul(GFX3D_ONE - ny, half_h));
    if (v->sx < ctx->vp_x) v->out |= GFX3D_OUT_LEFT;
    if (v->sx >= ctx->vp_x + ctx->vp_w) v->out |= GFX3D_OUT_RIGHT;
    if (v->sy < ctx->vp_y) v->out |= GFX3D_OUT_TOP;
    if (v->sy >= ctx->vp_y + ctx->vp_h) v->out |= GFX3D_OUT_BOTTOM;
}

/** Clip a triangle against the near plane; returns the polygon vertex count. */
static int clip_near(gfx3d_ctx_t *ctx, const gfx3d_vertex_t *in,
                     gfx3d_vertex_t *out)
{
    gfx3d_fixed plane = ctx->near_z;
    int n = 0;
    int i;

    for (i = 0; i < 3; i++) {
        const gfx3d_vertex_t *a = &in[i];
        const gfx3d_vertex_t *b = &in[(i + 1) % 3];
        bool ain = a->clip.w > plane;
        bool bin = b->clip.w > plane;

        if (ain && bin) {
            if (n < GFX3D_MAX_CLIP_VERTS - 1) out[n++] = *a;
        } else if (ain && !bin) {
            gfx3d_fixed t = gfx3d_fp_div(plane - a->clip.w,
                                         b->clip.w - a->clip.w);
            if (n < GFX3D_MAX_CLIP_VERTS - 1) out[n++] = *a;
            if (n < GFX3D_MAX_CLIP_VERTS - 1) {
                out[n++] = vertex_lerp(a, b, t);
            }
        } else if (!ain && bin) {
            gfx3d_fixed t = gfx3d_fp_div(plane - a->clip.w,
                                         b->clip.w - a->clip.w);
            if (n < GFX3D_MAX_CLIP_VERTS - 1) {
                out[n++] = vertex_lerp(a, b, t);
            }
        }
    }
    return n;
}

/** Twice the signed screen-space area; positive means front-facing with the
 * y-down screen and the right-handed view matrix used by gfx3d_camera(). */
static int64_t screen_area(const gfx3d_vertex_t *v)
{
    return (int64_t)(v[1].sx - v[0].sx) * (int64_t)(v[2].sy - v[0].sy) -
           (int64_t)(v[2].sx - v[0].sx) * (int64_t)(v[1].sy - v[0].sy);
}

/** Flat shade from the view-space face normal: returns the DOS ramp level. */
static int face_shade(const gfx3d_ctx_t *ctx, const gfx3d_vertex_t *v)
{
    gfx3d_vec3_t e1 = gfx3d_vec3_sub(v[1].view, v[0].view);
    gfx3d_vec3_t e2 = gfx3d_vec3_sub(v[2].view, v[0].view);
    gfx3d_vec3_t n = gfx3d_vec3_normalize(gfx3d_vec3_cross(e1, e2));
    gfx3d_fixed diffuse = gfx3d_vec3_dot(n, ctx->light_dir);
    gfx3d_fixed lit;
    int level;

    if (diffuse < 0) diffuse = 0;
    lit = ctx->ambient + gfx3d_fp_mul(GFX3D_ONE - ctx->ambient, diffuse);
    level = (int)(((int64_t)lit * GFX3D_SHADES) >> GFX3D_FP_SHIFT);
    if (level < 0) level = 0;
    if (level >= GFX3D_SHADES) level = GFX3D_SHADES - 1;
    return level;
}

/** Stable insertion sort, far to near; ties by id. No qsort context needed. */
static void sort_tris(gfx3d_tri_t *tris, int n)
{
    int i;

    for (i = 1; i < n; i++) {
        gfx3d_tri_t key = tris[i];
        int j = i - 1;

        while (j >= 0 && (tris[j].depth < key.depth ||
                          (tris[j].depth == key.depth &&
                           tris[j].id > key.id))) {
            tris[j + 1] = tris[j];
            j--;
        }
        tris[j + 1] = key;
    }
}

/* ------------------------------------------------------------------------
 * Render - one raster path
 * ------------------------------------------------------------------------ */

void gfx3d_render(gfx3d_ctx_t *ctx, gfx_surface_t *surface,
                  const gfx3d_mesh_t *mesh)
{
    char line[128];
    int face;
    int ntri = 0;

    if (ctx == NULL || surface == NULL || mesh == NULL || mesh->v == NULL ||
        mesh->f == NULL || !ctx->ready) {
        return;
    }
    memset(&ctx->stats, 0, sizeof(ctx->stats));
    ctx->stats.shade_bytes = (uint32_t)(16 * GFX3D_SHADES * sizeof(uint16_t));

    for (face = 0; face < mesh->nf; face++) {
        gfx3d_vertex_t src[3];
        gfx3d_vertex_t poly[GFX3D_MAX_CLIP_VERTS];
        uint16_t i0 = mesh->f[face * 3];
        uint16_t i1 = mesh->f[face * 3 + 1];
        uint16_t i2 = mesh->f[face * 3 + 2];
        int np;
        int k;

        ctx->stats.tris++;
        if (i0 >= (uint16_t)mesh->nv || i1 >= (uint16_t)mesh->nv ||
            i2 >= (uint16_t)mesh->nv) {
            ctx->stats.overflow++;
            continue;
        }
        src[0] = transform_vertex(ctx, mesh->v[i0]);
        src[1] = transform_vertex(ctx, mesh->v[i1]);
        src[2] = transform_vertex(ctx, mesh->v[i2]);
        ctx->stats.verts += 3;
        np = clip_near(ctx, src, poly);
        if (np < 3) {
            ctx->stats.clipped++;
            continue;
        }
        /* Fan-triangulate the clipped polygon (at most 4 vertices). */
        for (k = 1; k + 1 < np; k++) {
            gfx3d_vertex_t tri[3];
            gfx3d_fixed depth;
            int level;

            tri[0] = poly[0];
            tri[1] = poly[k];
            tri[2] = poly[k + 1];
            project_vertex(ctx, &tri[0]);
            project_vertex(ctx, &tri[1]);
            project_vertex(ctx, &tri[2]);
            if ((tri[0].out | tri[1].out | tri[2].out) & GFX3D_OUT_FAR) {
                ctx->stats.clipped++;
                continue;
            }
            if ((tri[0].out & GFX3D_OUT_LEFT && tri[1].out & GFX3D_OUT_LEFT &&
                 tri[2].out & GFX3D_OUT_LEFT) ||
                (tri[0].out & GFX3D_OUT_RIGHT && tri[1].out & GFX3D_OUT_RIGHT &&
                 tri[2].out & GFX3D_OUT_RIGHT) ||
                (tri[0].out & GFX3D_OUT_TOP && tri[1].out & GFX3D_OUT_TOP &&
                 tri[2].out & GFX3D_OUT_TOP) ||
                (tri[0].out & GFX3D_OUT_BOTTOM && tri[1].out & GFX3D_OUT_BOTTOM &&
                 tri[2].out & GFX3D_OUT_BOTTOM)) {
                ctx->stats.clipped++;
                continue;
            }
            if (ctx->cull && screen_area(tri) <= 0) {
                snprintf(line, sizeof(line),
                         "e3d.cull id=%d area=%lld sx=%d,%d,%d sy=%d,%d,%d",
                         face, (long long)screen_area(tri),
                         tri[0].sx, tri[1].sx, tri[2].sx,
                         tri[0].sy, tri[1].sy, tri[2].sy);
                emit_line(ctx, line);
                ctx->stats.culled++;
                continue;
            }
            if (ntri >= GFX3D_MAX_TRIS) {
                ctx->stats.overflow++;
                snprintf(line, sizeof(line), "e3d.overflow id=%d", face);
                emit_line(ctx, line);
                continue;
            }
            level = face_shade(ctx, tri);
            tri[0].shade = tri[1].shade = tri[2].shade =
                (gfx3d_fixed)((int64_t)level * GFX3D_ONE / GFX3D_SHADES);
            depth = (tri[0].depth + tri[1].depth + tri[2].depth) / 3;
            ctx->tris[ntri].v[0] = tri[0];
            ctx->tris[ntri].v[1] = tri[1];
            ctx->tris[ntri].v[2] = tri[2];
            ctx->tris[ntri].depth = depth;
            {
                uint8_t base = mesh->face_colors != NULL
                             ? (uint8_t)(mesh->face_colors[face] & 0x0F)
                             : (uint8_t)(mesh->dos_color & 0x0F);
                ctx->tris[ntri].color =
                    ctx->shades[base * GFX3D_SHADES + level];
            }
            ctx->tris[ntri].id = (uint16_t)face;
            snprintf(line, sizeof(line),
                     "e3d.tri id=%d depth=%d shade=%d color=%04x",
                     face, gfx3d_to_int(depth), level,
                     (unsigned)ctx->tris[ntri].color);
            emit_line(ctx, line);
            ntri++;
        }
    }
    sort_tris(ctx->tris, ntri);

    for (face = 0; face < ntri; face++) {
        const gfx3d_tri_t *t = &ctx->tris[face];
        int x0 = t->v[0].sx;
        int y0 = t->v[0].sy;
        int x1 = t->v[1].sx;
        int y1 = t->v[1].sy;
        int x2 = t->v[2].sx;
        int y2 = t->v[2].sy;

        ctx->stats.drawn++;
        ctx->stats.last_depth = t->depth;
        if (ctx->mode == GFX3D_WIRE) {
            gfx_surface_line(surface, x0, y0, x1, y1, t->color);
            gfx_surface_line(surface, x1, y1, x2, y2, t->color);
            gfx_surface_line(surface, x2, y2, x0, y0, t->color);
        } else {
            gfx_surface_triangle(surface, x0, y0, x1, y1, x2, y2,
                                 t->color, true);
        }
        snprintf(line, sizeof(line),
                 "e3d.draw id=%d depth=%d color=%04x sx=%d,%d,%d sy=%d,%d,%d",
                 (int)t->id, gfx3d_to_int(t->depth), (unsigned)t->color,
                 x0, x1, x2, y0, y1, y2);
        emit_line(ctx, line);
    }
    snprintf(line, sizeof(line),
             "e3d.stats verts=%d tris=%d clipped=%d culled=%d drawn=%d "
             "overflow=%d last_depth=%d shade_bytes=%u",
             ctx->stats.verts, ctx->stats.tris, ctx->stats.clipped,
             ctx->stats.culled, ctx->stats.drawn, ctx->stats.overflow,
             gfx3d_to_int(ctx->stats.last_depth),
             (unsigned)ctx->stats.shade_bytes);
    emit_line(ctx, line);
}
