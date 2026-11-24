/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file native_commands.c
 * @module command
 * @layer command
 * @domain shell
 * @provides native diagnostics verb, the single .bat launch app, and all boot seams
 * @status experimental
 * @brief Front doors and boot wiring for the one signed native payload loader.
 *
 * This file is the ONLY place that binds firmware facilities to
 * `components/nativeexec`. It installs:
 *   - SHA-256/ECDSA hooks into `nativepay` implemented with the same mbedTLS
 *     primitives as the manifest trust core;
 *   - the single `p4sign` public key loaded through `pkg_sign_pubkey_load()`
 *     (there is no second key store and no embedded key);
 *   - an executable-memory map hook (a capability request, not an MMU claim);
 *   - one versioned API table built from applib helpers.
 *
 * Two thin front doors share ONE core (`native_run_app` -> `native_exec_launch`):
 *   - the diagnostics verb `native info|verify <APP>|status`;
 *   - the `.bat`-facing app `natrun <APP> [args]` registered via `app_register`.
 * No verb launches an unsigned, malformed, unverified or unsupported payload.
 */

#include "p4minishell_config.h"
#include "shell.h"
#include "batch.h"
#include "command.h"
#include "storage.h"
#include "ansi_palette.h"
#include "p4heap.h"

#include "native_payload.h"
#include "nativeexec.h"
#include "applib.h"

#include "esp_heap_caps.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#if !defined(MBEDTLS_ECDSA_C) || !defined(MBEDTLS_ECP_C) || !defined(MBEDTLS_SHA256_C)
#error "native payload verification needs MBEDTLS_ECDSA_C + MBEDTLS_ECP_C + MBEDTLS_SHA256_C"
#endif

#define NATIVE_STATE_FILE "NATIVE.STATE.INI"
#define NATIVE_LAUNCH_APP "natrun"
#define NATIVE_LAUNCH_DESC "natrun <APP> [args] - launch one signed NATIVE/<APP>.P4X payload (one loader)"

/* ========================================================================
 * Crypto seams: the same P-256 / SHA-256 primitives as the pkg trust core.
 * ======================================================================== */

static int native_sha256_spans(const native_pay_span_t *spans, size_t span_count,
                               uint8_t digest_out[NATIVE_PAY_SHA256_BYTES])
{
    mbedtls_sha256_context ctx;
    size_t i;
    int rc = 1;

    if (spans == NULL || digest_out == NULL || span_count == 0u ||
        span_count > NATIVE_PAY_SPANS_MAX) {
        return 1;
    }
    mbedtls_sha256_init(&ctx);
    do {
        if (mbedtls_sha256_starts(&ctx, 0) != 0) {
            break;
        }
        for (i = 0; i < span_count; i++) {
            if (spans[i].data == NULL || spans[i].len == 0u) {
                continue;
            }
            if (mbedtls_sha256_update(&ctx, spans[i].data, spans[i].len) != 0) {
                break;
            }
        }
        if (i != span_count) {
            break;
        }
        if (mbedtls_sha256_finish(&ctx, digest_out) != 0) {
            break;
        }
        rc = 0;
    } while (0);
    mbedtls_sha256_free(&ctx);
    return rc;
}

/** Verify a pre-computed digest with raw r||s and raw X||Y. Mirrors
 *  pkg_sign_verify() but takes the digest the envelope already hashed, so the
 *  signed byte stream stays exactly the ABI.md §4.1 canonical bytes. */
static int native_ecdsa_verify_p256(const uint8_t digest[NATIVE_PAY_SHA256_BYTES],
                                    const uint8_t signature[NATIVE_PAY_SIG_BYTES],
                                    const uint8_t pubkey[NATIVE_PAY_PUBKEY_BYTES])
{
    mbedtls_ecp_group grp;
    mbedtls_ecp_point q;
    mbedtls_mpi r;
    mbedtls_mpi s;
    uint8_t point[1 + NATIVE_PAY_PUBKEY_BYTES];
    int rc = 1;

    if (digest == NULL || signature == NULL || pubkey == NULL) {
        return 1;
    }
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    do {
        if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) != 0) {
            break;
        }
        point[0] = 0x04;
        memcpy(point + 1, pubkey, NATIVE_PAY_PUBKEY_BYTES);
        if (mbedtls_ecp_point_read_binary(&grp, &q, point, sizeof(point)) != 0 ||
            mbedtls_ecp_check_pubkey(&grp, &q) != 0) {
            break;
        }
        if (mbedtls_mpi_read_binary(&r, signature, 32) != 0 ||
            mbedtls_mpi_read_binary(&s, signature + 32, 32) != 0) {
            break;
        }
        if (mbedtls_ecdsa_verify(&grp, digest, NATIVE_PAY_SHA256_BYTES,
                                 &q, &r, &s) != 0) {
            break;
        }
        rc = 0;
    } while (0);
    memset(point, 0, sizeof(point));
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&q);
    mbedtls_ecp_group_free(&grp);
    return rc;
}

/* ========================================================================
 * Executable-memory + API seams.
 * ======================================================================== */

/** A capability request for executable heap, not an MMU mapping. No per-app
 *  address space, relocation or isolation is claimed; failure is explicit. */
static void *native_map_payload(const uint8_t *payload, size_t size)
{
    void *code = NULL;

    if (payload == NULL || size == 0u) {
        return NULL;
    }
#if defined(MALLOC_CAP_EXEC)
    code = heap_caps_malloc(size, (uint32_t)MALLOC_CAP_EXEC | (uint32_t)MALLOC_CAP_8BIT);
    if (code == NULL) {
        code = heap_caps_malloc(size, (uint32_t)MALLOC_CAP_EXEC);
    }
#endif
    if (code == NULL) {
        return NULL;
    }
    memcpy(code, payload, size);
    /* Make the just-written bytes visible to instruction fetch (GCC emits the
     * target's icache fence where supported). Hardware execution is still a
     * board-verification step; this is only the required coherency hook. */
    __builtin___clear_cache((char *)code, (char *)code + size);
    return code;
}

static void native_unmap_payload(void *code, size_t size)
{
    (void)size;
    if (code != NULL) {
        heap_caps_free(code);
    }
}

static int native_api_print(const char *text)
{
    if (text == NULL) {
        return -1;
    }
    return app_printf("%s", text);
}

static const char *native_api_env_get(const char *key)
{
    return (key != NULL) ? app_env_get(key) : NULL;
}

static int native_api_env_set(const char *key, const char *value)
{
    if (key == NULL || value == NULL) {
        return 1;
    }
    return app_env_set(key, value) ? 0 : 1;
}

static int native_api_state_get(const char *key, char *out, size_t size)
{
    if (key == NULL || out == NULL || size == 0u) {
        return 1;
    }
    return app_ini_get(NATIVE_STATE_FILE, key, out, size) ? 0 : 1;
}

static int native_api_state_set(const char *key, const char *value)
{
    if (key == NULL || value == NULL) {
        return 1;
    }
    return app_ini_set(NATIVE_STATE_FILE, key, value) ? 0 : 1;
}

static uint32_t native_api_time_ms(void)
{
    return (uint32_t)app_now_ms();
}

static const native_exec_api_t s_native_api = {
    NATIVE_EXEC_API_VERSION,
    native_api_print,
    native_api_env_get,
    native_api_env_set,
    native_api_state_get,
    native_api_state_set,
    native_api_time_ms
};

int native_commands_init(void)
{
    static const native_pay_crypto_ops_t ops = {
        native_sha256_spans,
        native_ecdsa_verify_p256
    };
    uint8_t pub[NATIVE_PAY_PUBKEY_BYTES];

    /* Hooks are installed even when no key exists: the verifier then remains
     * explicitly "not ready", never an implicit pass. */
    native_pay_set_crypto_ops(&ops);
    if (pkg_sign_pubkey_load(pub) == 0) {
        native_pay_set_trusted_key(pub);
    } else {
        native_pay_set_trusted_key(NULL);
    }
    memset(pub, 0, sizeof(pub));

    native_exec_set_map_ops(native_map_payload, native_unmap_payload);
    native_exec_set_api(&s_native_api);
    return native_pay_verifier_ready() ? 0 : 1;
}

/* ========================================================================
 * One file reader + one run core shared by both front doors.
 * ======================================================================== */

static bool native_app_name_valid(const char *name)
{
    const unsigned char *p = (const unsigned char *)name;

    if (name == NULL || name[0] == '\0') {
        return false;
    }
    for (; *p != '\0'; p++) {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' ||
              *p == '.')) {
            return false;
        }
    }
    if (name[0] == '.' && name[1] == '\0') {
        return false;
    }
    return true;
}

/** Read one reserved envelope into a heap buffer. Returns 0 on success,
 *  1 on a bad name/path/SD error, 2 on a short or unreadable file. */
static int native_read_envelope(const char *app, uint8_t **buf_out, size_t *len_out)
{
    char rel[P4_CONFIG_SD_PATH_BYTES];
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    shell_sd_session_t session;
    size_t cap = (size_t)P4_CONFIG_NATPAY_HEADER_BYTES +
                 (size_t)P4_CONFIG_NATPAY_MAX_PAYLOAD_BYTES;
    uint8_t *buf;
    FILE *file;
    size_t got;

    if (buf_out == NULL || len_out == NULL) {
        return 1;
    }
    *buf_out = NULL;
    *len_out = 0u;
    if (!native_app_name_valid(app)) {
        return 1;
    }
    snprintf(rel, sizeof(rel), "NATIVE/%s.P4X", app);
    if (shell_fs_resolve_path(rel, resolved, sizeof(resolved)) != ESP_OK ||
        shell_sd_begin(&session) != ESP_OK) {
        return 1;
    }
    file = fopen(resolved, "rb");
    if (file == NULL) {
        shell_sd_end(&session, "native");
        return 1;
    }
    buf = (uint8_t *)p4heap_alloc_psram(cap);
    if (buf == NULL) {
        fclose(file);
        shell_sd_end(&session, "native");
        return 2;
    }
    got = fread(buf, 1u, cap, file);
    fclose(file);
    shell_sd_end(&session, "native");
    if (got < (size_t)P4_CONFIG_NATPAY_HEADER_BYTES) {
        p4heap_free(buf);
        return 2;
    }
    *buf_out = buf;
    *len_out = got;
    return 0;
}

/** The single run core. `verify_only` stops after inspection; otherwise the
 *  one loader path maps and calls the signed entry. */
static int native_run_app(const char *app, bool verify_only,
                          int argc, char **argv)
{
    uint8_t *buf = NULL;
    size_t len = 0u;
    native_exec_plan_t plan;
    int rc = native_read_envelope(app, &buf, &len);
    int entry_rc;

    if (rc != 0) {
        shell_transcript_appendf_ansi(
            SH_ERR "native: %s: %s\n" SH_RST, app,
            (rc == 1) ? "payload missing or unreadable" : "out of memory");
        return 1;
    }

    if (verify_only) {
        /* Diagnostics only: inspect once and never map or call anything. */
        entry_rc = native_exec_inspect(buf, len, &plan);
        if (entry_rc != NATIVE_EXEC_OK) {
            shell_transcript_appendf_ansi(
                SH_ERR "native: %s: %s (verdict: %s)\n" SH_RST, app,
                native_exec_status_name(entry_rc),
                native_pay_status_name((native_pay_status_t)plan.verify_status));
            p4heap_free(buf);
            return 1;
        }
        shell_transcript_appendf_ansi(
            SH_OK "native: %s: verified; api=0x%02lX; entry=%lu (not executed)\n" SH_RST,
            app, (unsigned long)plan.required_api_set,
            (unsigned long)plan.entry_ordinal);
        p4heap_free(buf);
        return 0;
    }

    /* Launch: the one loader verifies exactly once, then maps and calls. */
    entry_rc = native_exec_launch(buf, len, argc, argv);
    if (entry_rc < 0) {
        shell_transcript_appendf_ansi(
            SH_ERR "native: %s: %s\n" SH_RST, app,
            native_exec_status_name(entry_rc));
    }
    p4heap_free(buf);
    return (entry_rc < 0) ? 1 : entry_rc;
}

/* ========================================================================
 * Front door 1: the diagnostics verb (parent wires it into command.c/help).
 * ======================================================================== */

static const char *native_yes_no(bool value)
{
    return value ? "yes" : "no";
}

static void native_print_info(void)
{
    shell_transcript_appendf_ansi(
        SH_HEAD "native: signed payload loader\n" SH_RST);
    shell_transcript_appendf_ansi(
        SH_KV_FMT, "abi", P4_CONFIG_NATIVE_ABI);
    shell_transcript_appendf_ansi(
        SH_KV_FMT, "envelope",
        "P4NATPAY v1 / " NATIVE_PAY_ARCH " / " NATIVE_EXEC_EXPORT_MAGIC " v1");
    shell_transcript_appendf_ansi(
        SH_KV_FMT, "crypto hooks", native_yes_no(native_pay_crypto_installed()));
    shell_transcript_appendf_ansi(
        SH_KV_FMT, "trusted key", native_yes_no(native_pay_key_installed()));
    shell_transcript_appendf_ansi(
        SH_KV_FMT, "verifier ready", native_yes_no(native_pay_verifier_ready()));
    shell_transcript_appendf_ansi(
        SH_KV_FMT, "api provider", native_yes_no(native_exec_api_ready()));
    shell_transcript_appendf_ansi(
        SH_KV_FMT, "map hook", native_yes_no(native_exec_map_ready()));
    shell_transcript_appendf_ansi(
        "  supported api: 0x%02lX (console, env, state, time)\n",
        (unsigned long)P4_CONFIG_NATIVE_API_SUPPORTED_MASK);
    shell_transcript_appendf_ansi(
        "  max payload: %lu bytes; exports: %u\n",
        (unsigned long)P4_CONFIG_NATPAY_MAX_PAYLOAD_BYTES,
        (unsigned int)P4_CONFIG_NATIVE_MAX_EXPORTS);
}

void shell_command_native(int argc, char **argv)
{
    const char *sub;

    if (argc < 2) {
        shell_print_usage(
            "Usage: native info | status | verify <APP>  "
            "(launch a signed payload from .BAT with " NATIVE_LAUNCH_APP " <APP>)");
        batch_set_errorlevel(2);
        return;
    }
    sub = argv[1];
    if (strcasecmp(sub, "info") == 0 || strcasecmp(sub, "status") == 0) {
        native_print_info();
        batch_set_errorlevel(0);
        return;
    }
    if (strcasecmp(sub, "verify") == 0) {
        if (argc != 3) {
            shell_print_usage("Usage: native verify <APP>");
            batch_set_errorlevel(2);
            return;
        }
        if (!native_app_name_valid(argv[2])) {
            shell_transcript_appendf_ansi(SH_ERR "native: bad app name\n" SH_RST);
            batch_set_errorlevel(2);
            return;
        }
        batch_set_errorlevel(native_run_app(argv[2], true, 0, NULL));
        return;
    }
    shell_print_usage(
        "Usage: native info | status | verify <APP>");
    batch_set_errorlevel(2);
}

/* ========================================================================
 * Front door 2: the .bat-facing app (registered through app_register).
 * ======================================================================== */

static int native_launch_main(int argc, char **argv)
{
    if (argc < 2) {
        shell_print_usage("Usage: " NATIVE_LAUNCH_APP " <APP> [args]");
        return 2;
    }
    return native_run_app(argv[1], false, argc - 1, argv + 1);
}

bool native_launcher_register(void)
{
    return app_register(NATIVE_LAUNCH_APP, NATIVE_LAUNCH_DESC, native_launch_main);
}
