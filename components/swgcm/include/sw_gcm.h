/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/**
 * @file sw_gcm.h
 * @brief Small self-contained software AES-256-GCM (no hardware, no DMA).
 *
 * Fallback for `crypt` when the esp-aes hardware path cannot allocate its
 * GDMA descriptor array from the fragmented Tab5 internal DMA heap (see
 * bugs.md F23). Pure CPU code: buffers may live in PSRAM, and it needs no
 * internal RAM beyond the caller-provided context plus in/out buffers.
 *
 * Wire format matches the hardware path exactly (AES-256-GCM, 12-byte
 * nonce, 16-byte tag), so files sealed by either path open with the other.
 * Only the streaming subset `crypt` needs is exposed: init/setkey/starts
 * (12-byte nonce, encrypt or decrypt), update (CTR + GHASH), finish (tail
 * flush + tag). No AAD support (crypt uses none).
 */

#define SW_GCM_KEY_LEN 32
#define SW_GCM_NONCE_LEN 12
#define SW_GCM_TAG_LEN 16

typedef struct {
    uint32_t rk[60];      /**< AES-256 expanded key. */
    uint8_t h[16];        /**< GHASH subkey H = AES_K(0^128). */
    uint8_t y[16];        /**< Current counter block. */
    uint8_t y0[16];       /**< Initial counter (for the tag). */
    uint8_t ks[16];       /**< Current keystream block. */
    uint8_t ks_used;      /**< Keystream bytes consumed (0..16). */
    uint8_t x[16];        /**< GHASH accumulator. */
    uint8_t xbuf[16];     /**< Partial GHASH block buffer. */
    uint8_t xbuf_used;    /**< Bytes in @p xbuf (0..16). */
    uint64_t len_ct;      /**< Ciphertext bytes processed. */
    bool encrypt;         /**< Direction. */
    bool live;            /**< True between starts() and free(). */
} sw_gcm_ctx_t;

#ifdef __cplusplus
extern "C" {
#endif

void sw_gcm_init(sw_gcm_ctx_t *ctx);
int sw_gcm_setkey(sw_gcm_ctx_t *ctx, const uint8_t key[SW_GCM_KEY_LEN]);
int sw_gcm_starts(sw_gcm_ctx_t *ctx, bool encrypt, const uint8_t nonce[SW_GCM_NONCE_LEN]);

/**
 * Feed @p len bytes; @p olen_out receives bytes written to @p out.
 * Mirrors `crypt_gcm_update_all()`: partial trailing bytes are buffered
 * internally and flushed by `sw_gcm_finish()`.
 */
int sw_gcm_update(sw_gcm_ctx_t *ctx, const uint8_t *in, size_t len,
                  uint8_t *out, size_t *olen_out);

/**
 * Flush trailing bytes (into @p tail_out, at most 15) and compute @p tag.
 * Mirrors `crypt_gcm_finish_all()`.
 */
int sw_gcm_finish(sw_gcm_ctx_t *ctx, uint8_t *tail_out, size_t *tail_len_out,
                  uint8_t tag[SW_GCM_TAG_LEN]);

/** Zeroize the context (key schedule included). */
void sw_gcm_free(sw_gcm_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
