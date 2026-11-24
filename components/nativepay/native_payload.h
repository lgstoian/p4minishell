/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file native_payload.h
 * @module nativepay
 * @layer leaf
 * @domain app
 * @provides opt-in signed native payload envelope parsing and verification
 * @status experimental
 * @test test/main/test_native_payload.c
 * @hard-rule ai-context.md#module-layering-rules
 * @brief Pure parser/verifier for the reserved `NATIVE/<APP>.P4X` payload
 *        envelope (see `ABI.md` §4.1).
 *
 * This module is deliberately format-only. It reads a memory buffer, checks
 * the envelope, and reports a verdict. It has no filesystem, no ELF loader,
 * no command verbs, and never transfers control to payload bytes. The future
 * firmware loader installs the SHA-256/ECDSA hooks through
 * `native_pay_set_crypto_ops()` and the trust key through
 * `native_pay_set_trusted_key()`; until then verification degrades to an
 * explicit "no verifier" status, never to a silent success.
 *
 * The envelope is little-endian and self-describing:
 *
 * | Offset | Bytes | Field |
 * |--------|-------|-------|
 * | 0      | 8     | magic `P4NATPAY` |
 * | 8      | 4     | format version (u32, currently 1) |
 * | 12     | 8     | arch id, NUL-padded (`esp32p4`) |
 * | 20     | 16    | ABI tag, NUL-padded (`applib-1`) |
 * | 36     | 4     | payload size (u32) |
 * | 40     | 4     | payload CRC-32 (u32, integrity only) |
 * | 44     | 32    | payload SHA-256 |
 * | 76     | 64    | ECDSA P-256 signature, `r || s` |
 * | 140    | 2     | entry ordinal (u16, 0-based) |
 * | 142    | 2     | reserved (must be zero) |
 * | 144    | 4     | required API set (u32 bitmask) |
 * | 148    | N     | payload bytes (N = the size field) |
 *
 * Canonical signed bytes are the header with BOTH self-referential fields
 * omitted (the 32-byte SHA-256 field at 44 and the 64-byte signature field at
 * 76), immediately followed by the payload. That binds magic, version, arch,
 * ABI, size, CRC, entry ordinal, reserved, required API set and payload while
 * leaving no circular dependency on the digest or signature itself. SHA-256
 * over those bytes is the 32-byte digest the publisher signs. CRC-32 is a
 * cheap integrity check using the one shared primitive; it never substitutes
 * for the signature.
 */

#ifndef P4MINISHELL_NATIVE_PAYLOAD_H
#define P4MINISHELL_NATIVE_PAYLOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Fixed field offsets and lengths, in bytes (protocol constants). */
enum {
    NATIVE_PAY_OFF_MAGIC     = 0,
    NATIVE_PAY_OFF_VERSION   = 8,
    NATIVE_PAY_OFF_ARCH      = 12,
    NATIVE_PAY_OFF_ABI       = 20,
    NATIVE_PAY_OFF_SIZE      = 36,
    NATIVE_PAY_OFF_CRC32     = 40,
    NATIVE_PAY_OFF_SHA256    = 44,
    NATIVE_PAY_OFF_SIGNATURE = 76,
    NATIVE_PAY_OFF_ENTRY     = 140,
    NATIVE_PAY_OFF_RESERVED  = 142,
    NATIVE_PAY_OFF_API_SET   = 144,
    NATIVE_PAY_HEADER_LEN    = 148
};

/** Envelope constants. The ABI tag itself comes from `P4_CONFIG_NATIVE_ABI`. */
#define NATIVE_PAY_MAGIC           "P4NATPAY"
#define NATIVE_PAY_ARCH            "esp32p4"
#define NATIVE_PAY_FORMAT_VERSION  1u
#define NATIVE_PAY_SHA256_BYTES    32u
#define NATIVE_PAY_SIG_BYTES       64u
#define NATIVE_PAY_PUBKEY_BYTES    64u

/** Maximum scatter/gather segments passed to the hash hook. The canonical
 *  byte stream needs three: header prefix, header suffix, payload. */
#define NATIVE_PAY_SPANS_MAX       3u

/** Required API set bits. Advisory to the future loader: they describe which
 *  applib facilities the payload expects, not a new execution surface. */
#define NATIVE_PAY_API_CONSOLE     (1u << 0)
#define NATIVE_PAY_API_ENV         (1u << 1)
#define NATIVE_PAY_API_STATE       (1u << 2)
#define NATIVE_PAY_API_INPUT       (1u << 3)
#define NATIVE_PAY_API_TUI         (1u << 4)
#define NATIVE_PAY_API_MODAL       (1u << 5)
#define NATIVE_PAY_API_DB          (1u << 6)
#define NATIVE_PAY_API_TIME        (1u << 7)
#define NATIVE_PAY_API_WEBAPP      (1u << 8)
#define NATIVE_PAY_API_KNOWN_MASK  0x1FFu

/** Result codes. Zero only means "fully verified"; a missing verifier has its
 *  own code so a caller can never mistake degradation for a trust decision. */
typedef enum {
    NATIVE_PAY_OK           = 0,   /*< CRC, SHA-256 and signature all verified */
    NATIVE_PAY_ERR_ARG      = -1,  /*< NULL argument or malformed view */
    NATIVE_PAY_ERR_TOO_SMALL= -2,  /*< buffer shorter than the fixed header */
    NATIVE_PAY_ERR_MAGIC    = -3,  /*< magic mismatch */
    NATIVE_PAY_ERR_VERSION  = -4,  /*< unsupported format version */
    NATIVE_PAY_ERR_ARCH     = -5,  /*< arch field is not `esp32p4` */
    NATIVE_PAY_ERR_ABI      = -6,  /*< ABI tag is not `P4_CONFIG_NATIVE_ABI` */
    NATIVE_PAY_ERR_SIZE     = -7,  /*< payload size over cap or past buffer end */
    NATIVE_PAY_ERR_RESERVED = -8,  /*< reserved field is not zero */
    NATIVE_PAY_ERR_API_SET  = -9,  /*< unknown required-API bits */
    NATIVE_PAY_ERR_CRC      = -10, /*< payload CRC-32 mismatch */
    NATIVE_PAY_ERR_NO_VERIFIER = -11, /*< hooks or trust key not installed */
    NATIVE_PAY_ERR_HASH     = -12, /*< SHA-256 hook failed */
    NATIVE_PAY_ERR_SHA      = -13, /*< payload SHA-256 mismatch */
    NATIVE_PAY_ERR_SIGNATURE= -14  /*< ECDSA verification failed */
} native_pay_status_t;

/** One contiguous segment of the canonical byte stream. */
typedef struct {
    const uint8_t *data;
    size_t len;
} native_pay_span_t;

/** Firmware-installed crypto seam. Both hooks return 0 on success; anything
 *  else is a failure. NULL hooks are never called. */
typedef struct {
    int (*sha256_spans)(const native_pay_span_t *spans, size_t span_count,
                        uint8_t digest_out[NATIVE_PAY_SHA256_BYTES]);
    int (*ecdsa_verify_p256)(const uint8_t digest[NATIVE_PAY_SHA256_BYTES],
                             const uint8_t signature[NATIVE_PAY_SIG_BYTES],
                             const uint8_t pubkey[NATIVE_PAY_PUBKEY_BYTES]);
} native_pay_crypto_ops_t;

/** Read-only view into the caller's buffer. Every pointer aliases @p buf;
 *  nothing is copied and the buffer must outlive the view. */
typedef struct {
    const uint8_t *header;       /*< start of the fixed header */
    const uint8_t *payload;      /*< header + NATIVE_PAY_HEADER_LEN */
    size_t payload_size;         /*< payload byte count from the header */
    size_t total_len;            /*< buffer length the view was parsed from */
    const uint8_t *crc32_field;  /*< aliased little-endian header field */
    const uint8_t *sha256_field; /*< aliased 32-byte header field */
    const uint8_t *signature;    /*< aliased 64-byte header field */
    uint32_t format_version;     /*< decoded version field */
    uint16_t entry_ordinal;      /*< decoded 0-based entry ordinal */
    uint32_t required_api_set;   /*< decoded advisory API bitmask */
} native_pay_view_t;

/** Parse a buffer without doing any cryptography. Bounds-checks the whole
 *  envelope, including the payload tail, and rejects arch/ABI/version/
 *  reserved/unknown-API mismatches. Returns NATIVE_PAY_OK on a well-formed
 *  envelope; no trust decision is implied. */
int native_pay_parse(const uint8_t *buf, size_t len, native_pay_view_t *out);

/** Verify a parsed view with the installed crypto ops and trust key. The view
 *  is re-parsed internally so a caller cannot forge its fields. Checks CRC-32,
 *  the payload SHA-256, and the signature over the canonical bytes. Without
 *  both hooks and a key this returns NATIVE_PAY_ERR_NO_VERIFIER (after the
 *  CRC check), never NATIVE_PAY_OK. */
int native_pay_verify(const native_pay_view_t *view);

/** Convenience: parse then verify in one call. */
int native_pay_check(const uint8_t *buf, size_t len, native_pay_view_t *out);

/** Install (or, with NULL, clear) the crypto hooks. The struct is copied; the
 *  caller keeps ownership of its function pointers. Install once at boot. */
void native_pay_set_crypto_ops(const native_pay_crypto_ops_t *ops);

/** Install (or, with NULL, clear) the single trusted raw P-256 public key
 *  (`X || Y`, 64 bytes). The bytes are copied into module storage. */
void native_pay_set_trusted_key(const uint8_t pubkey[NATIVE_PAY_PUBKEY_BYTES]);

/** True when both crypto hooks are installed. Pure query; no crypto runs. */
bool native_pay_crypto_installed(void);

/** True when the single trusted key is installed. Pure query. */
bool native_pay_key_installed(void);

/** True only when hooks AND key are present, i.e. verification can succeed.
 *  Diagnostics must use this instead of guessing from a status code. */
bool native_pay_verifier_ready(void);

/** Stable diagnostic name for a status code (never NULL). */
const char *native_pay_status_name(native_pay_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_NATIVE_PAYLOAD_H */
