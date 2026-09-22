/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file sw_gcm.c
 * @brief Software AES-256-GCM streaming (NIST SP 800-38D, 12-byte nonces).
 */

#include "sw_gcm.h"

#include <string.h>

void sw_aes256_expand_key(const uint8_t key[32], uint32_t rk[60]);
void sw_aes256_encrypt_block(const uint32_t rk[60], const uint8_t in[16], uint8_t out[16]);

/** Multiply @p x by @p h in GF(2^128) (big-endian blocks). Result in @p x. */
static void s_gf_mult(const uint8_t h[16], uint8_t x[16])
{
    uint8_t z[16] = { 0 };
    uint8_t v[16];
    int i;
    int j;

    memcpy(v, h, 16);
    for (i = 0; i < 16; i++) {
        for (j = 7; j >= 0; j--) {
            if (x[i] & (uint8_t)(1u << j)) {
                int k;
                for (k = 0; k < 16; k++) {
                    z[k] ^= v[k];
                }
            }
            {
                uint8_t lsb = (uint8_t)(v[15] & 0x01);
                int k;
                for (k = 15; k > 0; k--) {
                    v[k] = (uint8_t)((v[k] >> 1) | (v[k - 1] << 7));
                }
                v[0] >>= 1;
                if (lsb) {
                    v[0] ^= 0xe1;
                }
            }
        }
    }
    memcpy(x, z, 16);
}

/** Absorb @p len ciphertext bytes into the GHASH accumulator. */
static void s_ghash_absorb(sw_gcm_ctx_t *ctx, const uint8_t *data, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        ctx->xbuf[ctx->xbuf_used++] ^= data[i];
        if (ctx->xbuf_used == 16) {
            int k;
            for (k = 0; k < 16; k++) {
                ctx->x[k] ^= ctx->xbuf[k];
                ctx->xbuf[k] = 0;
            }
            ctx->xbuf_used = 0;
            s_gf_mult(ctx->h, ctx->x);
        }
    }
}

/** Increment the low 32 bits of @p y (big-endian). */
static void s_incr32(uint8_t y[16])
{
    int i;
    for (i = 15; i >= 12; i--) {
        y[i]++;
        if (y[i] != 0) {
            break;
        }
    }
}

void sw_gcm_init(sw_gcm_ctx_t *ctx)
{
    if (ctx != NULL) {
        memset(ctx, 0, sizeof(*ctx));
    }
}

int sw_gcm_setkey(sw_gcm_ctx_t *ctx, const uint8_t key[SW_GCM_KEY_LEN])
{
    uint8_t zero[16];

    if (ctx == NULL || key == NULL) {
        return 1;
    }
    sw_aes256_expand_key(key, ctx->rk);
    memset(zero, 0, sizeof(zero));
    sw_aes256_encrypt_block(ctx->rk, zero, ctx->h);
    memset(zero, 0, sizeof(zero));
    return 0;
}

int sw_gcm_starts(sw_gcm_ctx_t *ctx, bool encrypt, const uint8_t nonce[SW_GCM_NONCE_LEN])
{
    if (ctx == NULL || nonce == NULL) {
        return 1;
    }
    memcpy(ctx->y0, nonce, SW_GCM_NONCE_LEN);
    memset(ctx->y0 + SW_GCM_NONCE_LEN, 0, 4);
    ctx->y0[15] = 1;
    memcpy(ctx->y, ctx->y0, 16);
    s_incr32(ctx->y); /* First data counter is Y0+1. */
    memset(ctx->ks, 0, sizeof(ctx->ks));
    ctx->ks_used = (uint8_t)sizeof(ctx->ks); /* Force keystream gen on first byte. */
    memset(ctx->x, 0, sizeof(ctx->x));
    memset(ctx->xbuf, 0, sizeof(ctx->xbuf));
    ctx->xbuf_used = 0;
    ctx->len_ct = 0;
    ctx->encrypt = encrypt;
    ctx->live = true;
    return 0;
}

int sw_gcm_update(sw_gcm_ctx_t *ctx, const uint8_t *in, size_t len,
                  uint8_t *out, size_t *olen_out)
{
    size_t i;

    if (olen_out != NULL) {
        *olen_out = 0;
    }
    if (ctx == NULL || !ctx->live) {
        return 1;
    }
    if (len == 0) {
        return 0;
    }
    if (in == NULL || out == NULL) {
        return 1;
    }
    for (i = 0; i < len; i++) {
        uint8_t c;
        if (ctx->ks_used == 16) {
            sw_aes256_encrypt_block(ctx->rk, ctx->y, ctx->ks);
            s_incr32(ctx->y);
            ctx->ks_used = 0;
        }
        c = (uint8_t)(in[i] ^ ctx->ks[ctx->ks_used++]);
        out[i] = c;
        /* GHASH always runs over the ciphertext. */
        s_ghash_absorb(ctx, ctx->encrypt ? &c : &in[i], 1);
    }
    ctx->len_ct += (uint64_t)len;
    if (olen_out != NULL) {
        *olen_out = len;
    }
    return 0;
}

int sw_gcm_finish(sw_gcm_ctx_t *ctx, uint8_t *tail_out, size_t *tail_len_out,
                  uint8_t tag[SW_GCM_TAG_LEN])
{
    uint8_t len_block[16];
    uint8_t s[16];
    uint64_t bit_len;
    int k;
    int i;

    if (ctx == NULL || tail_out == NULL || tail_len_out == NULL || tag == NULL) {
        return 1;
    }
    if (!ctx->live) {
        return 1;
    }
    /* No buffered plaintext/ciphertext tail exists (CTR emits every byte in
     * update), so the tail is always empty; only the tag is produced. */
    *tail_len_out = 0;
    /* Pad the final partial GHASH block with zeros (xbuf is zero-kept). */
    if (ctx->xbuf_used > 0) {
        for (k = 0; k < 16; k++) {
            ctx->x[k] ^= ctx->xbuf[k];
            ctx->xbuf[k] = 0;
        }
        ctx->xbuf_used = 0;
        s_gf_mult(ctx->h, ctx->x);
    }
    /* Length block: 64 zero bits (no AAD) + 64-bit big-endian bit length. */
    memset(len_block, 0, sizeof(len_block));
    bit_len = ctx->len_ct * 8u;
    for (i = 0; i < 8; i++) {
        len_block[8 + i] = (uint8_t)(bit_len >> (56 - 8 * i));
    }
    for (k = 0; k < 16; k++) {
        ctx->x[k] ^= len_block[k];
    }
    s_gf_mult(ctx->h, ctx->x);
    sw_aes256_encrypt_block(ctx->rk, ctx->y0, s);
    for (k = 0; k < SW_GCM_TAG_LEN; k++) {
        tag[k] = (uint8_t)(ctx->x[k] ^ s[k]);
    }
    memset(s, 0, sizeof(s));
    memset(len_block, 0, sizeof(len_block));
    ctx->live = false;
    return 0;
}

void sw_gcm_free(sw_gcm_ctx_t *ctx)
{
    if (ctx != NULL) {
        memset(ctx, 0, sizeof(*ctx));
    }
}
