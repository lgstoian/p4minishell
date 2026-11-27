/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 * @copyright Jason Howard. All rights reserved.
 *
 * Derived from jthwho/bmds sdk/3d/math.c (MIT, pinned commit ef05be9f;
 * full upstream licence text: tools/LICENSE-BMDS-MIT.txt). The column-major
 * layout, matrix construction order and transform/cofactor algorithms follow
 * that file; every operation is re-expressed here in Q16.16 fixed point with
 * an integer sine table, so no libm call remains on the game path. Only the
 * math is reused - BMDS raster, canvas, mesh, model and GUI code is not.
 */
/**
 * @file gfx3d_math.c
 * @brief Q16.16 vector/matrix math for the mini 3D engine.
 */

#include "gfx3d.h"

/* One full turn of sin(), stored at half Q16.16 amplitude
 * (round(sin(2*pi*i/256)*32768)) and doubled on read, so the table also fits
 * comfortably in int16 terms. A 256-entry table gives ~1.4 degree resolution,
 * ample for integer-degree batch verbs and identical on device and host. */
static const int32_t s_sin256[256] = {
         0,    804,   1608,   2411,   3212,   4011,   4808,   5602,
      6393,   7180,   7962,   8740,   9512,  10279,  11039,  11793,
     12540,  13279,  14010,  14733,  15447,  16151,  16846,  17531,
     18205,  18868,  19520,  20160,  20788,  21403,  22006,  22595,
     23170,  23732,  24279,  24812,  25330,  25833,  26320,  26791,
     27246,  27684,  28106,  28511,  28899,  29269,  29622,  29957,
     30274,  30572,  30853,  31114,  31357,  31581,  31786,  31972,
     32138,  32286,  32413,  32522,  32610,  32679,  32729,  32758,
     32768,  32758,  32729,  32679,  32610,  32522,  32413,  32286,
     32138,  31972,  31786,  31581,  31357,  31114,  30853,  30572,
     30274,  29957,  29622,  29269,  28899,  28511,  28106,  27684,
     27246,  26791,  26320,  25833,  25330,  24812,  24279,  23732,
     23170,  22595,  22006,  21403,  20788,  20160,  19520,  18868,
     18205,  17531,  16846,  16151,  15447,  14733,  14010,  13279,
     12540,  11793,  11039,  10279,   9512,   8740,   7962,   7180,
      6393,   5602,   4808,   4011,   3212,   2411,   1608,    804,
         0,   -804,  -1608,  -2411,  -3212,  -4011,  -4808,  -5602,
     -6393,  -7180,  -7962,  -8740,  -9512, -10279, -11039, -11793,
    -12540, -13279, -14010, -14733, -15447, -16151, -16846, -17531,
    -18205, -18868, -19520, -20160, -20788, -21403, -22006, -22595,
    -23170, -23732, -24279, -24812, -25330, -25833, -26320, -26791,
    -27246, -27684, -28106, -28511, -28899, -29269, -29622, -29957,
    -30274, -30572, -30853, -31114, -31357, -31581, -31786, -31972,
    -32138, -32286, -32413, -32522, -32610, -32679, -32729, -32758,
    -32768, -32758, -32729, -32679, -32610, -32522, -32413, -32286,
    -32138, -31972, -31786, -31581, -31357, -31114, -30853, -30572,
    -30274, -29957, -29622, -29269, -28899, -28511, -28106, -27684,
    -27246, -26791, -26320, -25833, -25330, -24812, -24279, -23732,
    -23170, -22595, -22006, -21403, -20788, -20160, -19520, -18868,
    -18205, -17531, -16846, -16151, -15447, -14733, -14010, -13279,
    -12540, -11793, -11039, -10279,  -9512,  -8740,  -7962,  -7180,
     -6393,  -5602,  -4808,  -4011,  -3212,  -2411,  -1608,   -804,
};

gfx3d_fixed gfx3d_sin_deg(int degrees)
{
    int d = degrees % 360;
    int idx;

    if (d < 0) d += 360;
    idx = (d * 256 + 180) / 360;
    if (idx >= 256) idx -= 256;
    return (gfx3d_fixed)s_sin256[idx] * 2;
}

gfx3d_fixed gfx3d_cos_deg(int degrees)
{
    return gfx3d_sin_deg(degrees + 90);
}

/** Integer restoring square root of a Q16.16 value; result is Q16.16. */
gfx3d_fixed gfx3d_isqrt_q16(gfx3d_fixed x)
{
    uint64_t n;
    uint64_t res = 0;
    uint64_t bit = UINT64_C(1) << 62;

    if (x <= 0) return 0;
    n = (uint64_t)x << 16;         /* workspace is the value scaled by 2^16 */
    while (bit > n) bit >>= 2;
    while (bit != 0) {
        if (n >= res + bit) {
            n -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return (gfx3d_fixed)res;       /* sqrt(x * 2^16) = sqrt(x) * 2^8 (Q16.16) */
}

/* ------------------------------------------------------------------------
 * Vectors
 * ------------------------------------------------------------------------ */

gfx3d_vec3_t gfx3d_vec3(gfx3d_fixed x, gfx3d_fixed y, gfx3d_fixed z)
{
    gfx3d_vec3_t r = {x, y, z};
    return r;
}

gfx3d_vec3_t gfx3d_vec3_add(gfx3d_vec3_t a, gfx3d_vec3_t b)
{
    return gfx3d_vec3(a.x + b.x, a.y + b.y, a.z + b.z);
}

gfx3d_vec3_t gfx3d_vec3_sub(gfx3d_vec3_t a, gfx3d_vec3_t b)
{
    return gfx3d_vec3(a.x - b.x, a.y - b.y, a.z - b.z);
}

gfx3d_vec3_t gfx3d_vec3_scale(gfx3d_vec3_t a, gfx3d_fixed k)
{
    return gfx3d_vec3(gfx3d_fp_mul(a.x, k), gfx3d_fp_mul(a.y, k),
                      gfx3d_fp_mul(a.z, k));
}

gfx3d_fixed gfx3d_vec3_dot(gfx3d_vec3_t a, gfx3d_vec3_t b)
{
    return gfx3d_fp_mul(a.x, b.x) + gfx3d_fp_mul(a.y, b.y) +
           gfx3d_fp_mul(a.z, b.z);
}

gfx3d_vec3_t gfx3d_vec3_cross(gfx3d_vec3_t a, gfx3d_vec3_t b)
{
    return gfx3d_vec3(gfx3d_fp_mul(a.y, b.z) - gfx3d_fp_mul(a.z, b.y),
                      gfx3d_fp_mul(a.z, b.x) - gfx3d_fp_mul(a.x, b.z),
                      gfx3d_fp_mul(a.x, b.y) - gfx3d_fp_mul(a.y, b.x));
}

gfx3d_fixed gfx3d_vec3_length(gfx3d_vec3_t a)
{
    return gfx3d_isqrt_q16(gfx3d_vec3_dot(a, a));
}

gfx3d_vec3_t gfx3d_vec3_normalize(gfx3d_vec3_t a)
{
    gfx3d_fixed l = gfx3d_vec3_length(a);

    if (l <= 0) return gfx3d_vec3(0, 0, 0);
    return gfx3d_vec3(gfx3d_fp_div(a.x, l), gfx3d_fp_div(a.y, l),
                      gfx3d_fp_div(a.z, l));
}

/* ------------------------------------------------------------------------
 * Matrices - column-major M(row,col) = m[col*4+row], as in the upstream file
 * ------------------------------------------------------------------------ */

#define M(mat, row, col) ((mat)->m[(col) * 4 + (row)])

void gfx3d_mat4_identity(gfx3d_mat4_t *m)
{
    int i;

    if (m == NULL) return;
    for (i = 0; i < 16; i++) m->m[i] = 0;
    m->m[0] = m->m[5] = m->m[10] = m->m[15] = GFX3D_ONE;
}

void gfx3d_mat4_multiply(gfx3d_mat4_t *out, const gfx3d_mat4_t *a,
                         const gfx3d_mat4_t *b)
{
    gfx3d_mat4_t r;
    int col;
    int row;

    if (out == NULL || a == NULL || b == NULL) return;
    for (col = 0; col < 4; ++col) {
        for (row = 0; row < 4; ++row) {
            M(&r, row, col) =
                gfx3d_fp_mul(M(a, row, 0), M(b, 0, col)) +
                gfx3d_fp_mul(M(a, row, 1), M(b, 1, col)) +
                gfx3d_fp_mul(M(a, row, 2), M(b, 2, col)) +
                gfx3d_fp_mul(M(a, row, 3), M(b, 3, col));
        }
    }
    *out = r;
}

void gfx3d_mat4_translation(gfx3d_mat4_t *m, gfx3d_fixed x,
                            gfx3d_fixed y, gfx3d_fixed z)
{
    gfx3d_mat4_identity(m);
    if (m == NULL) return;
    M(m, 0, 3) = x;
    M(m, 1, 3) = y;
    M(m, 2, 3) = z;
}

void gfx3d_mat4_scaling(gfx3d_mat4_t *m, gfx3d_fixed x,
                        gfx3d_fixed y, gfx3d_fixed z)
{
    gfx3d_mat4_identity(m);
    if (m == NULL) return;
    M(m, 0, 0) = x;
    M(m, 1, 1) = y;
    M(m, 2, 2) = z;
}

void gfx3d_mat4_rotation_x(gfx3d_mat4_t *m, int degrees)
{
    gfx3d_fixed c = gfx3d_cos_deg(degrees);
    gfx3d_fixed s = gfx3d_sin_deg(degrees);

    gfx3d_mat4_identity(m);
    if (m == NULL) return;
    M(m, 1, 1) = c;
    M(m, 1, 2) = -s;
    M(m, 2, 1) = s;
    M(m, 2, 2) = c;
}

void gfx3d_mat4_rotation_y(gfx3d_mat4_t *m, int degrees)
{
    gfx3d_fixed c = gfx3d_cos_deg(degrees);
    gfx3d_fixed s = gfx3d_sin_deg(degrees);

    gfx3d_mat4_identity(m);
    if (m == NULL) return;
    M(m, 0, 0) = c;
    M(m, 0, 2) = s;
    M(m, 2, 0) = -s;
    M(m, 2, 2) = c;
}

void gfx3d_mat4_rotation_z(gfx3d_mat4_t *m, int degrees)
{
    gfx3d_fixed c = gfx3d_cos_deg(degrees);
    gfx3d_fixed s = gfx3d_sin_deg(degrees);

    gfx3d_mat4_identity(m);
    if (m == NULL) return;
    M(m, 0, 0) = c;
    M(m, 0, 1) = -s;
    M(m, 1, 0) = s;
    M(m, 1, 1) = c;
}

void gfx3d_mat4_rotation_axis(gfx3d_mat4_t *m, gfx3d_vec3_t axis, int degrees)
{
    gfx3d_fixed l = gfx3d_vec3_length(axis);
    gfx3d_fixed x;
    gfx3d_fixed y;
    gfx3d_fixed z;
    gfx3d_fixed c;
    gfx3d_fixed s;
    gfx3d_fixed t;

    if (m == NULL) return;
    if (!(l > 0)) {
        gfx3d_mat4_identity(m);
        return;
    }
    x = gfx3d_fp_div(axis.x, l);
    y = gfx3d_fp_div(axis.y, l);
    z = gfx3d_fp_div(axis.z, l);
    c = gfx3d_cos_deg(degrees);
    s = gfx3d_sin_deg(degrees);
    t = GFX3D_ONE - c;
    gfx3d_mat4_identity(m);
    M(m, 0, 0) = gfx3d_fp_mul(t, gfx3d_fp_mul(x, x)) + c;
    M(m, 0, 1) = gfx3d_fp_mul(t, gfx3d_fp_mul(x, y)) - gfx3d_fp_mul(s, z);
    M(m, 0, 2) = gfx3d_fp_mul(t, gfx3d_fp_mul(x, z)) + gfx3d_fp_mul(s, y);
    M(m, 1, 0) = gfx3d_fp_mul(t, gfx3d_fp_mul(x, y)) + gfx3d_fp_mul(s, z);
    M(m, 1, 1) = gfx3d_fp_mul(t, gfx3d_fp_mul(y, y)) + c;
    M(m, 1, 2) = gfx3d_fp_mul(t, gfx3d_fp_mul(y, z)) - gfx3d_fp_mul(s, x);
    M(m, 2, 0) = gfx3d_fp_mul(t, gfx3d_fp_mul(x, z)) - gfx3d_fp_mul(s, y);
    M(m, 2, 1) = gfx3d_fp_mul(t, gfx3d_fp_mul(y, z)) + gfx3d_fp_mul(s, x);
    M(m, 2, 2) = gfx3d_fp_mul(t, gfx3d_fp_mul(z, z)) + c;
}

void gfx3d_mat4_perspective(gfx3d_mat4_t *m, int fovy_deg,
                            int aspect_num, int aspect_den,
                            gfx3d_fixed near_z, gfx3d_fixed far_z)
{
    gfx3d_fixed sin_half = gfx3d_sin_deg(fovy_deg / 2);
    gfx3d_fixed cos_half = gfx3d_cos_deg(fovy_deg / 2);
    gfx3d_fixed f;
    gfx3d_fixed aspect;
    int i;

    if (m == NULL) return;
    for (i = 0; i < 16; i++) m->m[i] = 0;
    if (sin_half == 0 || aspect_num == 0 || near_z == far_z) {
        gfx3d_mat4_identity(m);
        return;
    }
    /* f = 1/tan(fovy/2) = cos/sin as a plain ratio, expressed Q16.16. */
    f = (gfx3d_fixed)((int64_t)GFX3D_ONE * cos_half / sin_half);
    aspect = gfx3d_fp_div(gfx3d_from_int(aspect_num),
                          gfx3d_from_int(aspect_den));
    M(m, 0, 0) = gfx3d_fp_div(f, aspect);
    M(m, 1, 1) = f;
    M(m, 2, 2) = gfx3d_fp_div(far_z + near_z, near_z - far_z);
    M(m, 2, 3) = (gfx3d_fixed)((int64_t)2 * far_z * near_z / (near_z - far_z));
    M(m, 3, 2) = -GFX3D_ONE;
}

void gfx3d_mat4_ortho(gfx3d_mat4_t *m, gfx3d_fixed left,
                      gfx3d_fixed right, gfx3d_fixed bottom,
                      gfx3d_fixed top, gfx3d_fixed near_z,
                      gfx3d_fixed far_z)
{
    gfx3d_mat4_identity(m);
    if (m == NULL || right == left || top == bottom || far_z == near_z) return;
    M(m, 0, 0) = gfx3d_fp_div(2 * GFX3D_ONE, right - left);
    M(m, 1, 1) = gfx3d_fp_div(2 * GFX3D_ONE, top - bottom);
    M(m, 2, 2) = gfx3d_fp_div(-2 * GFX3D_ONE, far_z - near_z);
    M(m, 0, 3) = -gfx3d_fp_div(right + left, right - left);
    M(m, 1, 3) = -gfx3d_fp_div(top + bottom, top - bottom);
    M(m, 2, 3) = -gfx3d_fp_div(far_z + near_z, far_z - near_z);
}

void gfx3d_mat4_look_at(gfx3d_mat4_t *m, gfx3d_vec3_t eye,
                        gfx3d_vec3_t target, gfx3d_vec3_t up)
{
    gfx3d_vec3_t f = gfx3d_vec3_normalize(gfx3d_vec3_sub(target, eye));
    gfx3d_vec3_t s = gfx3d_vec3_normalize(gfx3d_vec3_cross(f, up));
    gfx3d_vec3_t u = gfx3d_vec3_cross(s, f);

    gfx3d_mat4_identity(m);
    if (m == NULL) return;
    M(m, 0, 0) = s.x;
    M(m, 0, 1) = s.y;
    M(m, 0, 2) = s.z;
    M(m, 1, 0) = u.x;
    M(m, 1, 1) = u.y;
    M(m, 1, 2) = u.z;
    M(m, 2, 0) = -f.x;
    M(m, 2, 1) = -f.y;
    M(m, 2, 2) = -f.z;
    M(m, 0, 3) = -gfx3d_vec3_dot(s, eye);
    M(m, 1, 3) = -gfx3d_vec3_dot(u, eye);
    M(m, 2, 3) = gfx3d_vec3_dot(f, eye);
}

int gfx3d_mat4_invert_affine(gfx3d_mat4_t *out, const gfx3d_mat4_t *m)
{
    gfx3d_fixed a;
    gfx3d_fixed b;
    gfx3d_fixed c;
    gfx3d_fixed d;
    gfx3d_fixed e;
    gfx3d_fixed f;
    gfx3d_fixed g;
    gfx3d_fixed h;
    gfx3d_fixed i;
    gfx3d_fixed A;
    gfx3d_fixed B;
    gfx3d_fixed C;
    gfx3d_fixed det;
    gfx3d_fixed k;
    gfx3d_mat4_t r;
    gfx3d_fixed tx;
    gfx3d_fixed ty;
    gfx3d_fixed tz;
    int row;

    if (out == NULL || m == NULL) return -1;
    a = M(m, 0, 0); b = M(m, 0, 1); c = M(m, 0, 2);
    d = M(m, 1, 0); e = M(m, 1, 1); f = M(m, 1, 2);
    g = M(m, 2, 0); h = M(m, 2, 1); i = M(m, 2, 2);
    A = gfx3d_fp_mul(e, i) - gfx3d_fp_mul(f, h);
    B = -(gfx3d_fp_mul(d, i) - gfx3d_fp_mul(f, g));
    C = gfx3d_fp_mul(d, h) - gfx3d_fp_mul(e, g);
    det = gfx3d_fp_mul(a, A) + gfx3d_fp_mul(b, B) + gfx3d_fp_mul(c, C);
    if (det == 0) return -1;
    k = gfx3d_fp_div(GFX3D_ONE, det);
    gfx3d_mat4_identity(&r);
    M(&r, 0, 0) = gfx3d_fp_mul(A, k);
    M(&r, 0, 1) = -gfx3d_fp_mul(gfx3d_fp_mul(b, i) - gfx3d_fp_mul(c, h), k);
    M(&r, 0, 2) = gfx3d_fp_mul(gfx3d_fp_mul(b, f) - gfx3d_fp_mul(c, e), k);
    M(&r, 1, 0) = gfx3d_fp_mul(B, k);
    M(&r, 1, 1) = gfx3d_fp_mul(gfx3d_fp_mul(a, i) - gfx3d_fp_mul(c, g), k);
    M(&r, 1, 2) = -gfx3d_fp_mul(gfx3d_fp_mul(a, f) - gfx3d_fp_mul(c, d), k);
    M(&r, 2, 0) = gfx3d_fp_mul(C, k);
    M(&r, 2, 1) = -gfx3d_fp_mul(gfx3d_fp_mul(a, h) - gfx3d_fp_mul(b, g), k);
    M(&r, 2, 2) = gfx3d_fp_mul(gfx3d_fp_mul(a, e) - gfx3d_fp_mul(b, d), k);
    tx = M(m, 0, 3); ty = M(m, 1, 3); tz = M(m, 2, 3);
    for (row = 0; row < 3; ++row) {
        M(&r, row, 3) = -(gfx3d_fp_mul(M(&r, row, 0), tx) +
                          gfx3d_fp_mul(M(&r, row, 1), ty) +
                          gfx3d_fp_mul(M(&r, row, 2), tz));
    }
    *out = r;
    return 0;
}

gfx3d_vec4_t gfx3d_mat4_transform(const gfx3d_mat4_t *m, gfx3d_vec3_t p)
{
    gfx3d_vec4_t r;

    r.x = gfx3d_fp_mul(M(m, 0, 0), p.x) + gfx3d_fp_mul(M(m, 0, 1), p.y) +
          gfx3d_fp_mul(M(m, 0, 2), p.z) + M(m, 0, 3);
    r.y = gfx3d_fp_mul(M(m, 1, 0), p.x) + gfx3d_fp_mul(M(m, 1, 1), p.y) +
          gfx3d_fp_mul(M(m, 1, 2), p.z) + M(m, 1, 3);
    r.z = gfx3d_fp_mul(M(m, 2, 0), p.x) + gfx3d_fp_mul(M(m, 2, 1), p.y) +
          gfx3d_fp_mul(M(m, 2, 2), p.z) + M(m, 2, 3);
    r.w = gfx3d_fp_mul(M(m, 3, 0), p.x) + gfx3d_fp_mul(M(m, 3, 1), p.y) +
          gfx3d_fp_mul(M(m, 3, 2), p.z) + M(m, 3, 3);
    return r;
}

gfx3d_vec3_t gfx3d_mat4_transform_point(const gfx3d_mat4_t *m, gfx3d_vec3_t p)
{
    gfx3d_vec4_t r = gfx3d_mat4_transform(m, p);
    gfx3d_fixed k = r.w != 0 ? gfx3d_fp_div(GFX3D_ONE, r.w) : GFX3D_ONE;

    return gfx3d_vec3(gfx3d_fp_mul(r.x, k), gfx3d_fp_mul(r.y, k),
                      gfx3d_fp_mul(r.z, k));
}

gfx3d_vec3_t gfx3d_mat4_transform_vector(const gfx3d_mat4_t *m, gfx3d_vec3_t v)
{
    return gfx3d_vec3(gfx3d_fp_mul(M(m, 0, 0), v.x) +
                          gfx3d_fp_mul(M(m, 0, 1), v.y) +
                          gfx3d_fp_mul(M(m, 0, 2), v.z),
                      gfx3d_fp_mul(M(m, 1, 0), v.x) +
                          gfx3d_fp_mul(M(m, 1, 1), v.y) +
                          gfx3d_fp_mul(M(m, 1, 2), v.z),
                      gfx3d_fp_mul(M(m, 2, 0), v.x) +
                          gfx3d_fp_mul(M(m, 2, 1), v.y) +
                          gfx3d_fp_mul(M(m, 2, 2), v.z));
}

#undef M
