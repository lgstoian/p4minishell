/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file certs.c
 * @brief TLS trust store: scan SD certificates, build a global CA store.
 *
 * The store lives in sd:/CERTS/.  PEM and DER files are accepted.  On load
 * the PEM contents are concatenated into a contiguous heap buffer and
 * registered with mbedTLS via `esp_tls_set_global_ca_store()`.  httpget and
 * c6ota then verify peer certificates against this combined store (user CAs
 * plus the compiled-in Mozilla bundle).  The `certs` shell command drives
 * add/remove/rebuild; the boot hook calls `certs_load_sd_store()` once after
 * the SD card first mounts.
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_tls.h"
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

/** Check whether a filename has the given extension (case-insensitive). */
static bool has_extension(const char *name, const char *ext)
{
    size_t name_len = strlen(name);
    size_t ext_len = strlen(ext);

    if (name_len < ext_len) {
        return false;
    }
    const char *suffix = name + name_len - ext_len;

    for (size_t i = 0; i < ext_len; i++) {
        char a = suffix[i];
        char b = ext[i];
        if (a >= 'A' && a <= 'Z') {
            a += 32;
        }
        if (b >= 'A' && b <= 'Z') {
            b += 32;
        }
        if (a != b) {
            return false;
        }
    }
    return true;
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

    /* Allocate the PEM buffer from PSRAM (can be up to 64 KiB). */
    if (s_pem_buf == NULL) {
        s_pem_buf = heap_caps_malloc(P4_CONFIG_TLS_CERTS_MAX_PEM_BYTES, MALLOC_CAP_SPIRAM);
    }
    if (s_pem_buf == NULL) {
        ESP_LOGE(CERTS_TAG, "failed to allocate PEM buffer (%d bytes)",
                 P4_CONFIG_TLS_CERTS_MAX_PEM_BYTES);
        return ESP_ERR_NO_MEM;
    }
    memset(s_pem_buf, 0, P4_CONFIG_TLS_CERTS_MAX_PEM_BYTES);

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

            /* Skip directories and the combined bundle. */
            if (entry->d_type == DT_DIR) {
                continue;
            }
            if (has_extension(name, ".pem")) {
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
            } else if (has_extension(name, ".der")) {
                der_count++;
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
     * length; it parses and caches the certificates internally. */
    error = esp_tls_set_global_ca_store(s_pem_buf, s_pem_len);
    if (error != ESP_OK) {
        ESP_LOGE(CERTS_TAG, "failed to set global CA store (%s)", esp_err_to_name(error));
        return error;
    }

    s_store_active = true;
    ESP_LOGI(CERTS_TAG, "loaded %d PEM file(s) (%d bytes) + %d DER file(s) "
             "into global CA store", pem_count, (int)s_pem_len, der_count);
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
        uint8_t buf[4096];

        while ((entry = readdir(dir)) != NULL) {
            const char *name = entry->d_name;

            if (entry->d_type == DT_DIR) {
                continue;
            }
            if (!has_extension(name, ".pem")) {
                continue;
            }

            /* Read and append each PEM file. */
            {
                char path[512];
                FILE *fp_in;

                snprintf(path, sizeof(path), "/sdcard/%s/%s",
                         P4_CONFIG_TLS_CERTS_DIR, name);
                fp_in = fopen(path, "rb");
                if (fp_in == NULL) {
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

        ESP_LOGI(CERTS_TAG, "rebuilt %s from %d PEM file(s)",
                 P4_CONFIG_TLS_CERTS_COMBINED_PEM, count);
    }

    shell_sd_end(&session, "certs rebuild");
    return ESP_OK;
}
