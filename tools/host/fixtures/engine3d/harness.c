/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file harness.c
 * @brief Host-only render harness for tools/host_engine3d_gate.py.
 *
 * Compiles the REAL gfx3d core plus the ONE gfx raster core and DOS palette.
 * It parses a tiny numeric scene fixture (the same shape the `gfx3d` batch
 * verbs accept), renders through gfx3d_render(), prints the core's deterministic
 * emit report and writes the resulting gfx_surface_t as a 24-bit BMP using the
 * one gfx_565_to_888_row() primitive. It never opens a serial port, fakes a
 * pixel or implements a second rasterizer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "gfx3d.h"
#include "ansi.h"

#define LINE_MAX 256
#define STEM_MAX 64

static char s_stem[STEM_MAX];

static void emit(const char *line, void *ud)
{
    (void)ud;
    printf("%s\n", line);
}

static void *ealloc(size_t n) { return malloc(n ? n : 1); }
static void erelease(void *p) { free(p); }
static uint32_t edos(uint8_t i) { return ansi_cga_color(i); }
static uint8_t enearest(uint32_t rgb) { return ansi_cga_nearest(rgb); }

/** Write a bottom-up 24-bit BI_RGB BMP of a gfx_surface_t. Serialization only:
 * the pixels come from the one RGB565 surface, converted by the one row helper. */
static int write_bmp(const char *path, const gfx_surface_t *s)
{
    FILE *f;
    uint32_t stride = (uint32_t)(((size_t)s->w * 3 + 3) & ~(size_t)3);
    uint32_t image = stride * (uint32_t)s->h;
    uint32_t total = 54 + image;
    unsigned char header[54];
    uint8_t *row;
    int y;

    f = fopen(path, "wb");
    if (f == NULL) return 0;
    memset(header, 0, sizeof(header));
    header[0] = 'B'; header[1] = 'M';
    header[2] = (unsigned char)(total & 0xFF);
    header[3] = (unsigned char)((total >> 8) & 0xFF);
    header[4] = (unsigned char)((total >> 16) & 0xFF);
    header[5] = (unsigned char)((total >> 24) & 0xFF);
    header[10] = 54;
    header[14] = 40;
    header[18] = (unsigned char)(s->w & 0xFF);
    header[19] = (unsigned char)((s->w >> 8) & 0xFF);
    header[22] = (unsigned char)(s->h & 0xFF);
    header[23] = (unsigned char)((s->h >> 8) & 0xFF);
    header[26] = 1;
    header[28] = 24;
    header[34] = (unsigned char)(image & 0xFF);
    header[35] = (unsigned char)((image >> 8) & 0xFF);
    header[36] = (unsigned char)((image >> 16) & 0xFF);
    header[37] = (unsigned char)((image >> 24) & 0xFF);
    if (fwrite(header, 1, sizeof(header), f) != sizeof(header)) {
        fclose(f);
        return 0;
    }
    row = (uint8_t *)malloc(stride);
    if (row == NULL) {
        fclose(f);
        return 0;
    }
    for (y = s->h - 1; y >= 0; y--) {
        gfx_565_to_888_row(row, s->px + (size_t)y * (size_t)s->w, s->w);
        if (fwrite(row, 1, stride, f) != stride) {
            free(row);
            fclose(f);
            return 0;
        }
    }
    free(row);
    fclose(f);
    return 1;
}

int main(int argc, char **argv)
{
    char line[LINE_MAX];
    FILE *fixture;
    gfx3d_ctx_t *ctx = NULL;
    gfx3d_ops_t ops = {ealloc, erelease, edos, enearest, emit, NULL};
    gfx3d_fixed verts[GFX3D_MAX_MESH_VERTS * 3];
    uint16_t faces[GFX3D_MAX_MESH_FACES * 3];
    uint8_t face_cols[GFX3D_MAX_MESH_FACES];
    char mesh_name[GFX3D_NAME_MAX] = "mesh";
    int mesh_color = 12;
    int nv = 0;
    int nf = 0;
    int w = 64;
    int h = 64;
    uint16_t *px = NULL;
    gfx_surface_t surface;
    int rc = 1;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <fixture.e3d> <outdir>\n", argv[0]);
        return 2;
    }
    fixture = fopen(argv[1], "r");
    if (fixture == NULL) {
        fprintf(stderr, "cannot open fixture %s\n", argv[1]);
        return 2;
    }
    strncpy(s_stem, argv[1], STEM_MAX - 1);
    {
        char *slash = strrchr(s_stem, '/');
        char *bslash = strrchr(s_stem, '\\');
        char *base = slash > bslash ? slash : bslash;
        char *dot;
        if (base != NULL) memmove(s_stem, base + 1, strlen(base + 1) + 1);
        dot = strrchr(s_stem, '.');
        if (dot != NULL) *dot = '\0';
    }

    ctx = (gfx3d_ctx_t *)ealloc(sizeof(*ctx));
    if (ctx == NULL || !gfx3d_init(ctx, &ops)) {
        fprintf(stderr, "engine init failed\n");
        goto done;
    }

    while (fgets(line, sizeof(line), fixture) != NULL) {
        char key[32];

        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        if (sscanf(line, "%31s", key) != 1) continue;

        if (strcmp(key, "canvas") == 0) {
            sscanf(line, "%*s %d %d", &w, &h);
        } else if (strcmp(key, "camera") == 0) {
            int e[3], t[3], u[3];
            if (sscanf(line, "%*s %d %d %d %d %d %d %d %d %d",
                       &e[0], &e[1], &e[2], &t[0], &t[1], &t[2],
                       &u[0], &u[1], &u[2]) == 9) {
                gfx3d_camera(ctx,
                    gfx3d_vec3(gfx3d_from_int(e[0]), gfx3d_from_int(e[1]), gfx3d_from_int(e[2])),
                    gfx3d_vec3(gfx3d_from_int(t[0]), gfx3d_from_int(t[1]), gfx3d_from_int(t[2])),
                    gfx3d_vec3(gfx3d_from_int(u[0]), gfx3d_from_int(u[1]), gfx3d_from_int(u[2])));
            }
        } else if (strcmp(key, "project") == 0) {
            int fovy, an, ad, near, far;
            if (sscanf(line, "%*s %d %d %d %d %d", &fovy, &an, &ad, &near, &far) == 5) {
                gfx3d_projection(ctx, fovy, an, ad, gfx3d_from_int(near),
                                 gfx3d_from_int(far));
            }
        } else if (strcmp(key, "light") == 0) {
            int x, y, z, ambient;
            if (sscanf(line, "%*s %d %d %d %d", &x, &y, &z, &ambient) == 4) {
                gfx3d_light(ctx,
                    gfx3d_vec3(gfx3d_from_int(x), gfx3d_from_int(y), gfx3d_from_int(z)),
                    ambient);
            }
        } else if (strcmp(key, "mode") == 0) {
            char m[16], c[16];
            int mode = GFX3D_SOLID;
            int cull = 1;
            if (sscanf(line, "%*s %15s %15s", m, c) >= 1) {
                if (strcmp(m, "wire") == 0) mode = GFX3D_WIRE;
                if (strcmp(c, "nocull") == 0) cull = 0;
            }
            gfx3d_mode(ctx, mode, cull);
        } else if (strcmp(key, "mesh") == 0) {
            sscanf(line, "%*s %15s %d", mesh_name, &mesh_color);
            nv = 0;
            nf = 0;
            memset(face_cols, 0xFF, sizeof(face_cols));
        } else if (strcmp(key, "c") == 0) {
            int fi;
            int color;
            if (sscanf(line, "%*s %d %d", &fi, &color) == 2 &&
                fi >= 0 && fi < GFX3D_MAX_MESH_FACES) {
                face_cols[fi] = (uint8_t)color;
            }
        } else if (strcmp(key, "end") == 0) {
            if (nv < 3 || nf < 1 ||
                !gfx3d_mesh_create(ctx, mesh_name, (uint8_t)mesh_color,
                                   nv, verts, nf, faces)) {
                fprintf(stderr, "mesh create failed (%s)\n", mesh_name);
                goto done;
            }
            {
                gfx3d_mesh_t *m = gfx3d_mesh_find(ctx, mesh_name);
                int fi;
                for (fi = 0; fi < nf; fi++) {
                    if (face_cols[fi] != 0xFF) {
                        gfx3d_mesh_face_color(ctx, m, fi, face_cols[fi]);
                    }
                }
            }
        } else if (strcmp(key, "v") == 0) {
            int x, y, z;
            if (sscanf(line, "%*s %d %d %d", &x, &y, &z) == 3 && nv < GFX3D_MAX_MESH_VERTS) {
                verts[nv * 3] = gfx3d_from_int(x);
                verts[nv * 3 + 1] = gfx3d_from_int(y);
                verts[nv * 3 + 2] = gfx3d_from_int(z);
                nv++;
            }
        } else if (strcmp(key, "f") == 0) {
            int a, b, c;
            if (sscanf(line, "%*s %d %d %d", &a, &b, &c) == 3 && nf < GFX3D_MAX_MESH_FACES) {
                faces[nf * 3] = (uint16_t)a;
                faces[nf * 3 + 1] = (uint16_t)b;
                faces[nf * 3 + 2] = (uint16_t)c;
                nf++;
            }
        } else if (strcmp(key, "draw") == 0) {
            char bmp[STEM_MAX + 8];
            char path[1024];

            if (w < 1 || w > GFX_MAX_W || h < 1 || h > GFX_MAX_H) {
                fprintf(stderr, "bad canvas %dx%d\n", w, h);
                goto done;
            }
            px = (uint16_t *)ealloc((size_t)w * (size_t)h * sizeof(*px));
            if (px == NULL) goto done;
            surface.px = px;
            surface.w = w;
            surface.h = h;
            gfx_surface_clear(&surface, 0x0000);
            gfx3d_viewport(ctx, 0, 0, w, h);
            {
                char draw_name[GFX3D_NAME_MAX];
                gfx3d_mesh_t *mesh;

                sscanf(line, "%*s %15s", draw_name);
                mesh = gfx3d_mesh_find(ctx, draw_name);
                if (mesh == NULL) {
                    fprintf(stderr, "no mesh %s\n", draw_name);
                    goto done;
                }
                printf("e3d.render.begin fixture=%s mesh=%s w=%d h=%d\n",
                       s_stem, draw_name, w, h);
                gfx3d_render(ctx, &surface, mesh);
            }
            snprintf(bmp, sizeof(bmp), "%s.bmp", s_stem);
            snprintf(path, sizeof(path), "%s/%s", argv[2], bmp);
            if (!write_bmp(path, &surface)) {
                fprintf(stderr, "BMP write failed: %s\n", path);
                goto done;
            }
            printf("e3d.frame path=%s\n", bmp);
            printf("e3d.render.end fixture=%s draws=%d culls=%d overflow=%d\n",
                   s_stem, ctx->stats.drawn, ctx->stats.culled,
                   ctx->stats.overflow);
            free(px);
            px = NULL;
        }
    }
    rc = 0;

done:
    if (px != NULL) free(px);
    gfx3d_close(ctx);
    free(ctx);
    fclose(fixture);
    return rc;
}
