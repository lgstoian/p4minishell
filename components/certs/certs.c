/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file certs.c
 * @brief TLS trust store: scan SD certificates, build a global CA store.
 *
 * The store lives in sd:/CERTS/.  PEM files are concatenated as-is; DER files
 * are validated and converted to PEM in memory (never stored converted).
 * The concatenated buffer is registered with mbedTLS via
 * `esp_tls_set_global_ca_store()`.  httpget and c6ota then verify peer
 * certificates against this combined store (user CAs plus the compiled-in
 * Mozilla bundle).  The `certs` shell command drives add/remove/rebuild; the
 * boot hook calls `certs_load_sd_store()` once after the SD card first
 * mounts.  The combined BUNDLE.PEM written by `certs rebuild` lives in the
 * same directory and is never ingested back.
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pem.h"
#include "p4minishell_config.h"
#include "storage.h"
#include "certs.h"

#define CERTS_TAG "certs"

/* ---- Internal state ---- */

/** Heap-allocated PEM buffer holding the concatenated certificate content. */
static uint8_t *s_pem_buf;
static size_t  s_pem_len;
static int     s_pem_files;
static int     s_der_files;
static bool    s_store_active;

/* ---- Helpers ---- */

/** True when @p name is the combined bundle file itself (which lives inside
 *  the scanned directory and must never be ingested, or every rebuild would
 *  double the store). Compared case-insensitively like the extension checks. */
static bool certs_is_combined_bundle(const char *name)
{
    const char *base = strrchr(P4_CONFIG_TLS_CERTS_COMBINED_PEM, '/');

    base = (base != NULL) ? base + 1 : P4_CONFIG_TLS_CERTS_COMBINED_PEM;
    return certs_name_equals_ci(name, base);
}

/** Read a whole file into a heap buffer, refusing files larger than @p cap.
 *  Returns the buffer (caller frees) with @p len_out set, or NULL. */
static uint8_t *certs_read_file_capped(const char *path, size_t cap, size_t *len_out)
{
    FILE *fp;
    long file_size;
    uint8_t *buf;
    size_t nread;

    if (len_out != NULL) {
        *len_out = 0;
    }
    fp = fopen(path, "rb");
    if (fp == NULL) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0 || (file_size = ftell(fp)) < 0) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);
    if (file_size == 0 || (size_t)file_size > cap) {
        fclose(fp);
        return NULL;
    }
    buf = heap_caps_malloc((size_t)file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        fclose(fp);
        return NULL;
    }
    nread = fread(buf, 1, (size_t)file_size, fp);
    fclose(fp);
    if (nread != (size_t)file_size) {
        heap_caps_free(buf);
        return NULL;
    }
    if (len_out != NULL) {
        *len_out = nread;
    }
    return buf;
}

/** Convert one DER certificate to PEM text. Pure (no SD access), unit-tested.
 *  Validates with mbedtls_x509_crt_parse_der first so garbage never reaches
 *  the trust store. */
esp_err_t certs_der_to_pem(const uint8_t *der, size_t der_len,
                           uint8_t *pem_out, size_t pem_cap, size_t *pem_len_out)
{
    mbedtls_x509_crt crt;
    int ret;
    size_t olen = 0;

    if (pem_len_out != NULL) {
        *pem_len_out = 0;
    }
    if (der == NULL || pem_out == NULL || pem_len_out == NULL || der_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    mbedtls_x509_crt_init(&crt);
    ret = mbedtls_x509_crt_parse_der(&crt, der, der_len);
    mbedtls_x509_crt_free(&crt);
    if (ret != 0) {
        return ESP_FAIL;
    }
    /* mbedtls_pem_write_buffer emits the header with NO trailing newline, so
     * the newline is part of the header argument; without it the PEM is one
     * fused line and esp_tls_set_global_ca_store refuses it (-0x2780). */
    ret = mbedtls_pem_write_buffer("-----BEGIN CERTIFICATE-----\n",
                                   "-----END CERTIFICATE-----",
                                   der, der_len, pem_out, pem_cap, &olen);
    if (ret != 0) {
        return ESP_ERR_NO_MEM;
    }
    *pem_len_out = olen;
    return ESP_OK;
}

/** Case-insensitive whole-name comparison. Kept local on purpose: the shared
 *  shell string helper lives in a component certs does not depend on, and a
 *  certs->shell dependency for one comparison would invert the layering. */
bool certs_name_equals_ci(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    while (*a != '\0' && *b != '\0') {
        char ca = *a++;
        char cb = *b++;

        if (ca >= 'A' && ca <= 'Z') {
            ca += 32;
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb += 32;
        }
        if (ca != cb) {
            return false;
        }
    }
    return *a == '\0' && *b == '\0';
}

/** Append the content of a file to the PEM buffer.  Returns the number of
 *  bytes appended (0 on error). */
static size_t certs_append_file(const char *path, size_t offset)
{
    FILE *fp;
    long file_size;
    size_t nread;
    uint8_t local_buf[4096];

    fp = fopen(path, "rb");
    if (fp == NULL) {
        ESP_LOGW(CERTS_TAG, "cannot open %s", path);
        return 0;
    }

    /* Determine file size. */
    if (fseek(fp, 0, SEEK_END) != 0 || (file_size = ftell(fp)) < 0) {
        fclose(fp);
        return 0;
    }
    rewind(fp);

    /* For small files, read directly into the buffer. For large files,
     * chunk through a local buffer (each chunk is the file content). */
    if ((size_t)file_size <= 4096) {
        nread = fread(local_buf, 1, (size_t)file_size, fp);
        fclose(fp);
        if (nread > 0 && offset + nread <= P4_CONFIG_TLS_CERTS_MAX_PEM_BYTES) {
            memcpy(s_pem_buf + offset, local_buf, nread);
            return nread;
        }
        return 0;
    }

    /* Large file: read in chunks. */
    {
        size_t remaining = (size_t)file_size;
        size_t pos = offset;

        while (remaining > 0) {
            size_t chunk = remaining > sizeof(local_buf) ? sizeof(local_buf) : remaining;

            if (pos + chunk > P4_CONFIG_TLS_CERTS_MAX_PEM_BYTES) {
                fclose(fp);
                return pos - offset;
            }
            nread = fread(local_buf, 1, chunk, fp);
            if (nread == 0) {
                break;
            }
            memcpy(s_pem_buf + pos, local_buf, nread);
            pos += nread;
            remaining -= nread;
        }
        fclose(fp);
        return pos - offset;
    }
}

/* ---- Public API ---- */

esp_err_t certs_load_sd_store(void)
{
    DIR *dir;
    struct dirent *entry;
    size_t pem_offset = 0;
    int pem_count = 0;
    int der_count = 0;
    esp_err_t error;

    /* Already loaded: nothing to do. */
    if (s_store_active) {
        return ESP_OK;
    }

    /* Ensure the CERTS directory exists on SD. */
    {
        shell_sd_session_t session;

        error = shell_sd_begin(&session);
        if (error != ESP_OK) {
            ESP_LOGI(CERTS_TAG, "SD not available, skipping cert store load");
            return error;
        }

        /* Create CERTS directory if it does not exist. */
        dir = opendir("/sdcard/" P4_CONFIG_TLS_CERTS_DIR);
        if (dir == NULL) {
            mkdir("/sdcard/" P4_CONFIG_TLS_CERTS_DIR, 0755);
            ESP_LOGI(CERTS_TAG, "created sd:/%s directory", P4_CONFIG_TLS_CERTS_DIR);
            shell_sd_end(&session, "certs init");
            s_store_active = false;
            return ESP_ERR_NOT_FOUND;
        }
        closedir(dir);
        shell_sd_end(&session, "certs scan");
    }

    /* Allocate the PEM buffer from PSRAM (can be up to 64 KiB, plus one
     * byte that permanently hosts the NUL terminator mbedTLS needs to
     * recognize PEM input). */
    if (s_pem_buf == NULL) {
        s_pem_buf = heap_caps_malloc(P4_CONFIG_TLS_CERTS_MAX_PEM_BYTES + 1, MALLOC_CAP_SPIRAM);
    }
    if (s_pem_buf == NULL) {
        ESP_LOGE(CERTS_TAG, "failed to allocate PEM buffer (%d bytes)",
                 P4_CONFIG_TLS_CERTS_MAX_PEM_BYTES + 1);
        return ESP_ERR_NO_MEM;
    }
    memset(s_pem_buf, 0, P4_CONFIG_TLS_CERTS_MAX_PEM_BYTES + 1);

    /* Second pass: read all .pem and .der files from sd:/CERTS. */
    {
        shell_sd_session_t session;

        error = shell_sd_begin(&session);
        if (error != ESP_OK) {
            return error;
        }

        dir = opendir("/sdcard/" P4_CONFIG_TLS_CERTS_DIR);
        if (dir == NULL) {
            shell_sd_end(&session, "certs readdir");
            return ESP_ERR_NOT_FOUND;
        }

        while ((entry = readdir(dir)) != NULL) {
            const char *name = entry->d_name;

            /* Skip directories and the combined bundle (which lives in this
             * directory and must never be ingested). */
            if (entry->d_type == DT_DIR || certs_is_combined_bundle(name)) {
                continue;
            }
            if (shell_path_has_extension(name, ".pem")) {
                char path[512];

                snprintf(path, sizeof(path), "/sdcard/%s/%s",
                         P4_CONFIG_TLS_CERTS_DIR, name);
                size_t appended = certs_append_file(path, pem_offset);

                if (appended > 0) {
                    pem_offset += appended;
                    pem_count++;
                } else {
                    ESP_LOGW(CERTS_TAG, "skipped empty/unreadable %s", name);
                }
            } else if (shell_path_has_extension(name, ".der")) {
                /* DER files are validated and converted to PEM in memory. */
                if (der_count >= P4_CONFIG_TLS_CERTS_MAX_DER_FILES) {
                    ESP_LOGW(CERTS_TAG, "skipped %s (DER file cap %d reached)",
                             name, P4_CONFIG_TLS_CERTS_MAX_DER_FILES);
                    continue;
                }
                {
                    char path[512];
                    uint8_t *der;
                    size_t der_len = 0;
                    size_t pem_len = 0;

                    snprintf(path, sizeof(path), "/sdcard/%s/%s",
                             P4_CONFIG_TLS_CERTS_DIR, name);
                    der = certs_read_file_capped(path, P4_CONFIG_TLS_CERTS_MAX_DER_BYTES,
                                                 &der_len);
                    if (der == NULL) {
                        ESP_LOGW(CERTS_TAG, "skipped unreadable/oversize %s", name);
                        continue;
                    }
                    if (certs_der_to_pem(der, der_len,
                                         s_pem_buf + pem_offset,
                                         P4_CONFIG_TLS_CERTS_MAX_PEM_BYTES - pem_offset,
                                         &pem_len) == ESP_OK && pem_len > 0) {
                        pem_offset += pem_len;
                        der_count++;
                    } else {
                        ESP_LOGW(CERTS_TAG, "skipped invalid DER %s", name);
                    }
                    heap_caps_free(der);
                }
            }
        }
        closedir(dir);
        shell_sd_end(&session, "certs load");
    }

    s_pem_len = pem_offset;
    s_pem_files = pem_count;
    s_der_files = der_count;

    if (pem_count == 0 && der_count == 0) {
        ESP_LOGI(CERTS_TAG, "no certificates found in sd:/%s",
                 P4_CONFIG_TLS_CERTS_DIR);
        return ESP_ERR_NOT_FOUND;
    }

    /* Register the PEM buffer with mbedTLS as the global CA store.
     * esp_tls_set_global_ca_store() takes a PEM-encoded buffer and its
     * length; it parses and caches the certificates internally.
     * Length MUST include the NUL terminator: mbedTLS sniffs PEM-vs-DER by
     * testing buf[len-1] == '\0', and anything else is DER-parsed (a PEM
     * file loaded without its terminator fails with -0x2180). The +1 byte
     * past the content cap permanently hosts it; the converter path already
     * counts it in olen. */
    s_pem_buf[pem_offset] = '\0';
    s_pem_len = pem_offset + 1;
    error = esp_tls_set_global_ca_store(s_pem_buf, s_pem_len);
    if (error != ESP_OK) {
        ESP_LOGE(CERTS_TAG, "failed to set global CA store (%s)", esp_err_to_name(error));
        return error;
    }

    s_store_active = true;
    ESP_LOGI(CERTS_TAG, "loaded %d PEM + %d DER file(s) (%d bytes) "
             "into global CA store", pem_count, der_count, (int)s_pem_len);
    return ESP_OK;
}

esp_err_t certs_reload(void)
{
    certs_clear();
    return certs_load_sd_store();
}

void certs_get_store_info(certs_store_info_t *out)
{
    if (out == NULL) {
        return;
    }
    out->pem_bytes = s_pem_len;
    out->pem_files = s_pem_files;
    out->der_files = s_der_files;
    out->global_store_active = s_store_active;
    out->sd_available = (storage_sd_is_mounted());
}

void certs_clear(void)
{
    /* Release the mbedTLS side first: freeing our buffer alone would leave
     * peer verification running against freed memory. */
    if (s_store_active) {
        esp_tls_free_global_ca_store();
    }
    if (s_pem_buf != NULL) {
        free(s_pem_buf);
        s_pem_buf = NULL;
    }
    s_pem_len = 0;
    s_pem_files = 0;
    s_der_files = 0;
    s_store_active = false;
}

bool certs_is_loaded(void)
{
    return s_store_active;
}

esp_err_t certs_rebuild_bundle(void)
{
    DIR *dir;
    struct dirent *entry;
    FILE *fp_out;
    shell_sd_session_t session;
    esp_err_t error;

    error = shell_sd_begin(&session);
    if (error != ESP_OK) {
        return error;
    }

    dir = opendir("/sdcard/" P4_CONFIG_TLS_CERTS_DIR);
    if (dir == NULL) {
        shell_sd_end(&session, "certs rebuild opendir");
        return ESP_ERR_NOT_FOUND;
    }

    /* Write the concatenated PEM bundle. */
    {
        char out_path[256];

        snprintf(out_path, sizeof(out_path), "/sdcard/%s", P4_CONFIG_TLS_CERTS_COMBINED_PEM);
        fp_out = fopen(out_path, "wb");
    }
    if (fp_out == NULL) {
        closedir(dir);
        shell_sd_end(&session, "certs rebuild fopen");
        return ESP_ERR_NO_MEM;
    }

    {
        int count = 0;
        int der_seen = 0;
        uint8_t buf[4096];

        while ((entry = readdir(dir)) != NULL) {
            const char *name = entry->d_name;

            /* Never ingest the bundle we are writing. */
            if (entry->d_type == DT_DIR || certs_is_combined_bundle(name)) {
                continue;
            }
            if (!shell_path_has_extension(name, ".pem") &&
                !shell_path_has_extension(name, ".der")) {
                continue;
            }

            /* Read and append each certificate (DER is converted to PEM). */
            {
                char path[512];
                FILE *fp_in;

                snprintf(path, sizeof(path), "/sdcard/%s/%s",
                         P4_CONFIG_TLS_CERTS_DIR, name);
                fp_in = fopen(path, "rb");
                if (fp_in == NULL) {
                    continue;
                }

                if (shell_path_has_extension(name, ".der")) {
                    /* DER files convert through a heap buffer (bounded). */
                    uint8_t *der;
                    size_t der_len = 0;
                    uint8_t *pem;
                    size_t pem_len = 0;

                    fclose(fp_in);
                    if (der_seen >= P4_CONFIG_TLS_CERTS_MAX_DER_FILES) {
                        ESP_LOGW(CERTS_TAG, "rebuild skipped %s (DER file cap reached)",
                                 name);
                        continue;
                    }
                    der = certs_read_file_capped(path, P4_CONFIG_TLS_CERTS_MAX_DER_BYTES,
                                                 &der_len);
                    if (der == NULL) {
                        continue;
                    }
                    pem = heap_caps_malloc(P4_CONFIG_TLS_CERTS_MAX_DER_BYTES * 2,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    if (pem != NULL &&
                        certs_der_to_pem(der, der_len, pem,
                                         P4_CONFIG_TLS_CERTS_MAX_DER_BYTES * 2,
                                         &pem_len) == ESP_OK && pem_len > 0) {
                        fwrite(pem, 1, pem_len, fp_out);
                        count++;
                        der_seen++;
                    }
                    heap_caps_free(pem);
                    heap_caps_free(der);
                    continue;
                }

                {
                    size_t nread;

                    while ((nread = fread(buf, 1, sizeof(buf), fp_in)) > 0) {
                        fwrite(buf, 1, nread, fp_out);
                    }
                }
                fclose(fp_in);
                count++;
            }
        }
        closedir(dir);
        fclose(fp_out);

        if (count == 0) {
            /* Remove the empty bundle file. */
            remove("/sdcard/" P4_CONFIG_TLS_CERTS_COMBINED_PEM);
            shell_sd_end(&session, "certs rebuild empty");
            return ESP_ERR_NOT_FOUND;
        }

        ESP_LOGI(CERTS_TAG, "rebuilt %s from %d certificate file(s)",
                 P4_CONFIG_TLS_CERTS_COMBINED_PEM, count);
    }

    shell_sd_end(&session, "certs rebuild");
    return ESP_OK;
}
