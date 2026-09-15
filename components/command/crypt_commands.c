/**
 * @file crypt_commands.c
 * @brief `crypt` — password file encryption (AES-256-GCM + PBKDF2-SHA256).
 *
 * The memo-password analogue for the SD card: `crypt lock <src> <dst>
 * [/p:pass | /ask]` seals a file, `crypt unlock` opens it. Secrets never
 * print (only byte counts), password buffers are zeroed after use, and an
 * inline `/p:` password is masked in the transcript echo and history by the
 * shell core (the `wifi connect` precedent).
 *
 * Format: `P4CRYPT1` magic + 16-byte salt + 12-byte nonce + ciphertext +
 * 16-byte GCM tag. The key comes from PBKDF2-HMAC-SHA256 over the password.
 * Files stream in 4 KB chunks through internal (DMA-safe) buffers, so
 * multi-megabyte files never touch PSRAM pointers in FATFS calls. Writes
 * are atomic (temp + rename with the FATFS no-overwrite dance) behind a
 * free-space pre-check; a failed run removes the partial destination.
 * A tag mismatch reports "wrong password or corrupt file" (never
 * distinguished) and also removes the partial.
 *
 * Batch-friendly: ERRORLEVEL 0 ok, 1 crypto/IO failure, 2 usage. The
 * interactive `/ask` prompt refuses headless (no key source) like `set /p`.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "batch.h"
#include "command.h"
#include "shell.h"
#include "storage.h"
#include "p4minishell_config.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"

#ifndef P4_CONFIG_CRYPT_CHUNK_BYTES
#define P4_CONFIG_CRYPT_CHUNK_BYTES 4096
#endif

#ifndef P4_CONFIG_CRYPT_PBKDF2_ITERS
#define P4_CONFIG_CRYPT_PBKDF2_ITERS 10000
#endif

#ifndef P4_CONFIG_CRYPT_SALT_BYTES
#define P4_CONFIG_CRYPT_SALT_BYTES 16
#endif

#ifndef P4_CONFIG_CRYPT_PASS_BYTES
#define P4_CONFIG_CRYPT_PASS_BYTES 128
#endif

#define CRYPT_MAGIC "P4CRYPT1"
#define CRYPT_MAGIC_LEN 8
#define CRYPT_NONCE_LEN 12
#define CRYPT_TAG_LEN 16
#define CRYPT_KEY_LEN 32
#define CRYPT_HEADER_LEN (CRYPT_MAGIC_LEN + P4_CONFIG_CRYPT_SALT_BYTES + CRYPT_NONCE_LEN)

static void shell_command_crypt_usage(void)
{
    shell_print_usage("Usage: crypt lock|unlock <src> <dst> [/p:pass | /ask]");
}

/* ========================================================================
 * Pure core (unit-tested, no SD / transcript)
 * ======================================================================== */

int crypt_derive_key(const char *pass, const uint8_t *salt, uint8_t *key_out)
{
    size_t pass_len;

    if (pass == NULL || salt == NULL || key_out == NULL) {
        return 1;
    }
    pass_len = strlen(pass);
    if (pass_len == 0 || pass_len >= (size_t)P4_CONFIG_CRYPT_PASS_BYTES) {
        return 1;
    }
    /* The `_ext` form is the non-deprecated entry point on this IDF port. */
    if (mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
                                      (const unsigned char *)pass, pass_len,
                                      salt, P4_CONFIG_CRYPT_SALT_BYTES,
                                      (unsigned int)P4_CONFIG_CRYPT_PBKDF2_ITERS,
                                      CRYPT_KEY_LEN, key_out) != 0) {
        return 1;
    }
    return 0;
}

/* The IDF hardware GCM port buffers partial blocks internally: update
 * reports how many bytes it emitted, and finish flushes up to 15 trailing
 * bytes into its own scratch plus the tag. Both helpers below honor that
 * shape so no tail byte is ever dropped. */

/** Feed @p len bytes; @p olen_out receives the bytes written to @p out. */
static int crypt_gcm_update_all(mbedtls_gcm_context *ctx, const uint8_t *in, size_t len,
                                uint8_t *out, size_t *olen_out)
{
    size_t olen = 0;

    if (olen_out != NULL) {
        *olen_out = 0;
    }
    if (len == 0) {
        return 0;
    }
    if (mbedtls_gcm_update(ctx, in, len, out, len, &olen) != 0 || olen > len) {
        return 1;
    }
    if (olen_out != NULL) {
        *olen_out = olen;
    }
    return 0;
}

/** Flush the trailing bytes (into @p tail_out, at most 15) and the tag. */
static int crypt_gcm_finish_all(mbedtls_gcm_context *ctx,
                                uint8_t *tail_out, size_t *tail_len_out,
                                uint8_t *tag)
{
    size_t tail_len = 0;

    if (tail_out == NULL || tail_len_out == NULL || tag == NULL) {
        return 1;
    }
    *tail_len_out = 0;
    if (mbedtls_gcm_finish(ctx, tail_out, 16, &tail_len, tag, CRYPT_TAG_LEN) != 0 ||
        tail_len > 16) {
        return 1;
    }
    *tail_len_out = tail_len;
    return 0;
}

/** One-shot GCM (single session, whole buffer). @p encrypt selects direction;
 *  decrypt returns 1 on tag mismatch. */
static int crypt_gcm_oneshot(bool encrypt, const uint8_t *key, const uint8_t *nonce,
                             const uint8_t *in, size_t len,
                             uint8_t *out, uint8_t *tag)
{
    mbedtls_gcm_context ctx;
    uint8_t tail[16];
    size_t olen = 0;
    size_t tail_len = 0;
    int rc = 1;

    mbedtls_gcm_init(&ctx);
    if (mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, CRYPT_KEY_LEN * 8) != 0) {
        goto done;
    }
    if (mbedtls_gcm_starts(&ctx, encrypt ? MBEDTLS_GCM_ENCRYPT : MBEDTLS_GCM_DECRYPT,
                            nonce, CRYPT_NONCE_LEN) != 0) {
        goto done;
    }
    if (crypt_gcm_update_all(&ctx, in, len, out, &olen) != 0) {
        goto done;
    }
    if (crypt_gcm_finish_all(&ctx, tail, &tail_len, tag) != 0) {
        goto done;
    }
    if (olen + tail_len != len) {
        goto done;
    }
    memcpy(out + olen, tail, tail_len);
    rc = 0;
done:
    mbedtls_gcm_free(&ctx);
    memset(tail, 0, sizeof(tail));
    return rc;
}

int crypt_encrypt_mem(const uint8_t *in, size_t len, const char *pass,
                      uint8_t *out, size_t out_size, size_t *out_len)
{
    uint8_t salt[P4_CONFIG_CRYPT_SALT_BYTES];
    uint8_t nonce[CRYPT_NONCE_LEN];
    uint8_t key[CRYPT_KEY_LEN];
    uint32_t word;
    int i;

    if (in == NULL || pass == NULL || out == NULL || out_len == NULL) {
        return 1;
    }
    if (out_size < CRYPT_HEADER_LEN + len + CRYPT_TAG_LEN) {
        return 1;
    }
    for (i = 0; i < P4_CONFIG_CRYPT_SALT_BYTES; i += 4) {
        word = esp_random();
        memcpy(salt + i, &word, 4);
    }
    for (i = 0; i < CRYPT_NONCE_LEN; i += 4) {
        word = esp_random();
        memcpy(nonce + i, &word, (size_t)((CRYPT_NONCE_LEN - i) >= 4 ? 4 : CRYPT_NONCE_LEN - i));
    }
    if (crypt_derive_key(pass, salt, key) != 0) {
        memset(key, 0, sizeof(key));
        return 1;
    }
    memcpy(out, CRYPT_MAGIC, CRYPT_MAGIC_LEN);
    memcpy(out + CRYPT_MAGIC_LEN, salt, sizeof(salt));
    memcpy(out + CRYPT_MAGIC_LEN + sizeof(salt), nonce, sizeof(nonce));
    if (crypt_gcm_oneshot(true, key, nonce, in, len,
                          out + CRYPT_HEADER_LEN, out + CRYPT_HEADER_LEN + len) != 0) {
        memset(key, 0, sizeof(key));
        return 1;
    }
    memset(key, 0, sizeof(key));
    *out_len = CRYPT_HEADER_LEN + len + CRYPT_TAG_LEN;
    return 0;
}

int crypt_decrypt_mem(const uint8_t *in, size_t len, const char *pass,
                      uint8_t *out, size_t out_size, size_t *out_len)
{
    const uint8_t *salt;
    const uint8_t *nonce;
    const uint8_t *cipher;
    const uint8_t *tag;
    size_t cipher_len;
    uint8_t key[CRYPT_KEY_LEN];
    uint8_t check[CRYPT_TAG_LEN];

    if (in == NULL || pass == NULL || out == NULL || out_len == NULL) {
        return 1;
    }
    if (len < CRYPT_HEADER_LEN + CRYPT_TAG_LEN ||
        memcmp(in, CRYPT_MAGIC, CRYPT_MAGIC_LEN) != 0) {
        return 1;
    }
    salt = in + CRYPT_MAGIC_LEN;
    nonce = salt + P4_CONFIG_CRYPT_SALT_BYTES;
    cipher = in + CRYPT_HEADER_LEN;
    cipher_len = len - CRYPT_HEADER_LEN - CRYPT_TAG_LEN;
    tag = in + CRYPT_HEADER_LEN + cipher_len;
    if (out_size < cipher_len) {
        return 1;
    }
    if (crypt_derive_key(pass, salt, key) != 0) {
        memset(key, 0, sizeof(key));
        return 1;
    }
    /* Decrypt into @p out, then verify the tag in constant shape (the
     * caller learns only ok / not-ok, never which byte failed). */
    if (crypt_gcm_oneshot(false, key, nonce, cipher, cipher_len, out, check) != 0) {
        memset(key, 0, sizeof(key));
        memset(out, 0, cipher_len);
        return 1;
    }
    memset(key, 0, sizeof(key));
    {
        uint8_t diff = 0;
        size_t i;
        for (i = 0; i < CRYPT_TAG_LEN; i++) {
            diff |= (uint8_t)(check[i] ^ tag[i]);
        }
        if (diff != 0) {
            memset(out, 0, cipher_len);
            memset(check, 0, sizeof(check));
            return 1;
        }
    }
    memset(check, 0, sizeof(check));
    *out_len = cipher_len;
    return 0;
}

/* ========================================================================
 * Password collection
 * ======================================================================== */

/** Collect the password: inline `/p:` wins, else a hidden `/ask` prompt.
 *  @return true with @p out filled (caller zeroes after use). */
static bool crypt_collect_password(const char *inline_pass, bool ask,
                                   char *out, size_t out_size)
{
    if (inline_pass != NULL) {
        if (inline_pass[0] == '\0' || strlen(inline_pass) >= out_size) {
            shell_print_error("crypt: password must be 1..%d characters",
                              (int)out_size - 1);
            return false;
        }
        snprintf(out, out_size, "%s", inline_pass);
        return true;
    }
    if (!ask) {
        shell_command_crypt_usage();
        return false;
    }
    shell_transcript_append_text("crypt password: ");
    if (!shell_read_line_hidden(out, out_size, P4_CONFIG_KEY_WAIT_TIMEOUT_MS)) {
        shell_transcript_append_text("\n");
        shell_print_error("crypt: password entry cancelled or no input source");
        return false;
    }
    shell_transcript_append_text("\n");
    if (out[0] == '\0') {
        shell_print_error("crypt: password must be 1..%d characters", (int)out_size - 1);
        return false;
    }
    return true;
}

/* ========================================================================
 * File verbs (streaming GCM session, atomic temp+rename)
 * ======================================================================== */

static int crypt_run_file(bool lock, const char *src, const char *dst,
                          const char *inline_pass, bool ask)
{
    char resolved_src[P4_CONFIG_SD_PATH_BYTES];
    char resolved_dst[P4_CONFIG_SD_PATH_BYTES];
    char tmp[P4_CONFIG_SD_PATH_BYTES + 8];
    char pass[P4_CONFIG_CRYPT_PASS_BYTES];
    uint8_t salt[P4_CONFIG_CRYPT_SALT_BYTES];
    uint8_t nonce[CRYPT_NONCE_LEN];
    uint8_t key[CRYPT_KEY_LEN];
    uint8_t tag[CRYPT_TAG_LEN];
    uint8_t *inbuf = NULL;
    uint8_t *outbuf = NULL;
    shell_sd_session_t session;
    FILE *fin = NULL;
    FILE *fout = NULL;
    mbedtls_gcm_context ctx;
    uint64_t total = 0;
    int rc = 1;
    bool ctx_live = false;
    uint32_t word;
    int i;

    if (shell_fs_resolve_path(src, resolved_src, sizeof(resolved_src)) != ESP_OK ||
        shell_fs_resolve_path(dst, resolved_dst, sizeof(resolved_dst)) != ESP_OK) {
        shell_print_error("crypt: invalid path");
        return 2;
    }
    if (!crypt_collect_password(inline_pass, ask, pass, sizeof(pass))) {
        return (inline_pass == NULL && !ask) ? 2 : 1;
    }
    if (shell_sd_begin(&session) != ESP_OK) {
        shell_print_error("crypt: SD card not present - insert and retry");
        memset(pass, 0, sizeof(pass));
        free(inbuf);
        free(outbuf);
        return 2;
    }
    fin = fopen(resolved_src, "rb");
    if (fin == NULL) {
        shell_print_error("crypt: cannot open %s", resolved_src);
        goto done_files;
    }
    /* Free-space pre-check: ciphertext runs ~45 bytes over plaintext. */
    {
        uint64_t needed = storage_get_file_size(resolved_src) + 512;
        uint64_t reclaim = storage_get_file_size(resolved_dst);
        if (!storage_check_free_space(needed, reclaim, "crypt")) {
            shell_print_error("crypt: not enough free space for %s", resolved_dst);
            goto done_files;
        }
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", resolved_dst);
    fout = fopen(tmp, "wb");
    if (fout == NULL) {
        shell_print_error("crypt: cannot write %s", tmp);
        goto done_files;
    }
    inbuf = malloc(P4_CONFIG_CRYPT_CHUNK_BYTES);
    outbuf = malloc(P4_CONFIG_CRYPT_CHUNK_BYTES);
    if (inbuf == NULL || outbuf == NULL) {
        shell_print_error("crypt: out of memory");
        goto done_files;
    }
    mbedtls_gcm_init(&ctx);
    ctx_live = true;
    if (lock) {
        for (i = 0; i < P4_CONFIG_CRYPT_SALT_BYTES; i += 4) {
            word = esp_random();
            memcpy(salt + i, &word, 4);
        }
        for (i = 0; i < CRYPT_NONCE_LEN; i += 4) {
            word = esp_random();
            memcpy(nonce + i, &word, (size_t)((CRYPT_NONCE_LEN - i) >= 4 ? 4 : CRYPT_NONCE_LEN - i));
        }
        if (crypt_derive_key(pass, salt, key) != 0) {
            shell_print_error("crypt: bad password");
            goto done_files;
        }
        if (mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, CRYPT_KEY_LEN * 8) != 0 ||
            mbedtls_gcm_starts(&ctx, MBEDTLS_GCM_ENCRYPT, nonce, CRYPT_NONCE_LEN) != 0) {
            shell_print_error("crypt: crypto init failed");
            goto done_files;
        }
        if (fwrite(CRYPT_MAGIC, 1, CRYPT_MAGIC_LEN, fout) != CRYPT_MAGIC_LEN ||
            fwrite(salt, 1, sizeof(salt), fout) != sizeof(salt) ||
            fwrite(nonce, 1, sizeof(nonce), fout) != sizeof(nonce)) {
            shell_print_error("crypt: write failed");
            goto done_files;
        }
        for (;;) {
            size_t n = fread(inbuf, 1, P4_CONFIG_CRYPT_CHUNK_BYTES, fin);
            size_t olen = 0;
            if (n > 0) {
                if (crypt_gcm_update_all(&ctx, inbuf, n, outbuf, &olen) != 0 ||
                    fwrite(outbuf, 1, olen, fout) != olen) {
                    shell_print_error("crypt: write failed");
                    goto done_files;
                }
                total += (uint64_t)olen;
            }
            if (n < P4_CONFIG_CRYPT_CHUNK_BYTES) {
                break;
            }
        }
        {
            uint8_t tail[16];
            size_t tail_len = 0;
            if (crypt_gcm_finish_all(&ctx, tail, &tail_len, tag) != 0) {
                memset(tail, 0, sizeof(tail));
                shell_print_error("crypt: write failed");
                goto done_files;
            }
            if ((tail_len > 0 && fwrite(tail, 1, tail_len, fout) != tail_len) ||
                fwrite(tag, 1, sizeof(tag), fout) != sizeof(tag)) {
                memset(tail, 0, sizeof(tail));
                shell_print_error("crypt: write failed");
                goto done_files;
            }
            total += (uint64_t)tail_len;
            memset(tail, 0, sizeof(tail));
        }
    } else {
        uint8_t header[CRYPT_HEADER_LEN];
        uint8_t check[CRYPT_TAG_LEN];
        uint8_t diff = 0;
        size_t n;
        long data_end;
        long chunk_end;

        /* Header first (magic + salt + nonce), then the tag is the file's
         * last 16 bytes; the middle is ciphertext. Sizes come from stat,
         * never fseek(SEEK_END)+ftell (the FATFS VFS misreports that way). */
        if (fread(header, 1, sizeof(header), fin) != sizeof(header) ||
            memcmp(header, CRYPT_MAGIC, CRYPT_MAGIC_LEN) != 0) {
            shell_print_error("crypt: %s is not a locked file", resolved_src);
            goto done_files;
        }
        {
            uint64_t size = storage_get_file_size(resolved_src);
            if (size < CRYPT_HEADER_LEN + CRYPT_TAG_LEN) {
                shell_print_error("crypt: %s is truncated", resolved_src);
                goto done_files;
            }
            data_end = (long)(size - CRYPT_TAG_LEN);
        }
        memcpy(salt, header + CRYPT_MAGIC_LEN, sizeof(salt));
        memcpy(nonce, header + CRYPT_MAGIC_LEN + sizeof(salt), sizeof(nonce));
        if (crypt_derive_key(pass, salt, key) != 0) {
            shell_print_error("crypt: bad password");
            goto done_files;
        }
        if (mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, CRYPT_KEY_LEN * 8) != 0 ||
            mbedtls_gcm_starts(&ctx, MBEDTLS_GCM_DECRYPT, nonce, CRYPT_NONCE_LEN) != 0) {
            shell_print_error("crypt: crypto init failed");
            goto done_files;
        }
        /* Position past the header; ciphertext runs to data_end. */
        if (fseek(fin, CRYPT_HEADER_LEN, SEEK_SET) != 0) {
            shell_print_error("crypt: read failed");
            goto done_files;
        }
        chunk_end = CRYPT_HEADER_LEN;
        for (;;) {
            long want = data_end - chunk_end;
            if (want <= 0) {
                break;
            }
            if (want > (long)P4_CONFIG_CRYPT_CHUNK_BYTES) {
                want = (long)P4_CONFIG_CRYPT_CHUNK_BYTES;
            }
            n = fread(inbuf, 1, (size_t)want, fin);
            if (n == 0) {
                shell_print_error("crypt: read failed");
                goto done_files;
            }
            {
                size_t olen = 0;
                if (crypt_gcm_update_all(&ctx, inbuf, n, outbuf, &olen) != 0 ||
                    fwrite(outbuf, 1, olen, fout) != olen) {
                    shell_print_error("crypt: write failed");
                    goto done_files;
                }
                total += (uint64_t)olen;
            }
            chunk_end += (long)n;
        }
        /* Read the trailing tag, flush the tail, and verify (constant shape). */
        if (fseek(fin, data_end, SEEK_SET) != 0 ||
            fread(tag, 1, sizeof(tag), fin) != sizeof(tag)) {
            shell_print_error("crypt: read failed");
            goto done_files;
        }
        {
            uint8_t tail[16];
            size_t tail_len = 0;
            if (crypt_gcm_finish_all(&ctx, tail, &tail_len, check) != 0) {
                memset(tail, 0, sizeof(tail));
                shell_print_error("crypt: read failed");
                goto done_files;
            }
            if (tail_len > 0 && fwrite(tail, 1, tail_len, fout) != tail_len) {
                memset(tail, 0, sizeof(tail));
                shell_print_error("crypt: write failed");
                goto done_files;
            }
            total += (uint64_t)tail_len;
            memset(tail, 0, sizeof(tail));
        }
        for (i = 0; i < CRYPT_TAG_LEN; i++) {
            diff |= (uint8_t)(check[i] ^ tag[i]);
        }
        memset(check, 0, sizeof(check));
        if (diff != 0) {
            shell_print_error("crypt: wrong password or corrupt file");
            goto done_files;
        }
    }
    if (fflush(fout) != 0 || fclose(fout) != 0) {
        fout = NULL;
        shell_print_error("crypt: write failed");
        goto done_files;
    }
    fout = NULL;
    fclose(fin);
    fin = NULL;
    /* FATFS f_rename refuses to overwrite an existing target. */
    if (rename(tmp, resolved_dst) != 0) {
        remove(resolved_dst);
        if (rename(tmp, resolved_dst) != 0) {
            remove(tmp);
            shell_print_error("crypt: cannot publish %s", resolved_dst);
            shell_sd_end(&session, "crypt");
            goto done_buffers;
        }
    }
    shell_sd_end(&session, "crypt");
    shell_print_ok("crypt: %s %llu byte(s) -> %s", lock ? "locked" : "unlocked",
                   (unsigned long long)total, resolved_dst);
    rc = 0;
    goto done_buffers;

done_files:
    if (fout != NULL) {
        fclose(fout);
    }
    if (fin != NULL) {
        fclose(fin);
    }
    remove(tmp);
    shell_sd_end(&session, "crypt");
    goto done_buffers;

done_buffers:
    if (ctx_live) {
        mbedtls_gcm_free(&ctx);
    }
    memset(key, 0, sizeof(key));
    memset(tag, 0, sizeof(tag));
    memset(pass, 0, sizeof(pass));
    free(inbuf);
    free(outbuf);
    return rc;
}

int shell_command_crypt(int argc, char **argv)
{
    bool lock;
    const char *src = NULL;
    const char *dst = NULL;
    const char *inline_pass = NULL;
    bool ask = false;
    int i;

    if (argc < 2) {
        shell_command_crypt_usage();
        return 2;
    }
    if (strcasecmp(argv[1], "lock") == 0) {
        lock = true;
    } else if (strcasecmp(argv[1], "unlock") == 0) {
        lock = false;
    } else {
        shell_command_crypt_usage();
        return 2;
    }
    for (i = 2; i < argc; i++) {
        const char *arg = argv[i];
        if (arg == NULL || arg[0] == '\0') {
            continue;
        }
        if (strncasecmp(arg, "/p:", 3) == 0) {
            if (inline_pass != NULL || ask) {
                shell_command_crypt_usage();
                return 2;
            }
            inline_pass = arg + 3;
        } else if (strcasecmp(arg, "/ask") == 0) {
            if (inline_pass != NULL || ask) {
                shell_command_crypt_usage();
                return 2;
            }
            ask = true;
        } else if (src == NULL) {
            src = arg;
        } else if (dst == NULL) {
            dst = arg;
        } else {
            shell_command_crypt_usage();
            return 2;
        }
    }
    if (src == NULL || dst == NULL) {
        shell_command_crypt_usage();
        return 2;
    }
    return crypt_run_file(lock, src, dst, inline_pass, ask);
}
