/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file epub_render.c
 * @brief EPUB chapter rendering using HTML renderer.
 */

#include "epub.h"
#include "epub_internal.h"
#include "html.h"
#include "zip.h"
#include "storage.h"
#include "p4minishell_config.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ------------------------------------------------------------------------- */
/* Chapter rendering                                                         */
/* ------------------------------------------------------------------------- */

esp_err_t epub_render_chapter(const epub_handle_t *handle,
                              char *out_buf,
                              size_t buf_size,
                              size_t *out_len)
{
    if (!handle || handle->current_spine_index < 0) return ESP_ERR_INVALID_STATE;

    const epub_chapter_t *ch = &handle->spine[handle->current_spine_index];
    if (!ch || !ch->href[0]) return ESP_ERR_INVALID_STATE;

    /* Build full path to chapter HTML */
    char chapter_path[EPUB_PATH_MAX];
    if (!epub_resolve_href(handle->opf_dir, ch->href, chapter_path, sizeof(chapter_path))) {
        return ESP_FAIL;
    }

    /* Extract chapter HTML to temp file */
    zip_entry_t entry;
    if (zip_find_entry(((epub_t *)handle)->zip_handle, chapter_path, &entry) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s/chapter.html", ((epub_t *)handle)->temp_dir);
    if (zip_extract_entry(((epub_t *)handle)->zip_handle, &entry, tmp_path) != ESP_OK) {
        return ESP_FAIL;
    }

    /* Read HTML file */
    FILE *f = fopen(tmp_path, "r");
    if (!f) return ESP_FAIL;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > P4_CONFIG_HTML_RENDER_MAX_BYTES) { fclose(f); return ESP_FAIL; }

    char *html = malloc(len + 1);
    if (!html) { fclose(f); return ESP_ERR_NO_MEM; }
    size_t read = fread(html, 1, len, f);
    fclose(f);
    html[read] = '\0';

    /* Render HTML to ANSI */
    size_t len_out = html_render_ansi(html, out_buf, buf_size);
    free(html);

    if (out_len) *out_len = len_out;
    return (len_out < buf_size || out_buf == NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

/* Resource extraction */
esp_err_t epub_extract_resource(const epub_handle_t *handle,
                                const char *href,
                                const char *dest_path)
{
    if (!handle || !href || !dest_path) return ESP_ERR_INVALID_ARG;

    char resource_path[EPUB_PATH_MAX];
    if (!epub_resolve_href(handle->opf_dir, href, resource_path, sizeof(resource_path))) {
        return ESP_ERR_NOT_FOUND;
    }

    void *zip = ((epub_t *)handle)->zip_handle;
    zip_entry_t entry;
    if (zip_find_entry(((epub_t *)handle)->zip_handle, resource_path, &entry) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    return zip_extract_entry(zip, &entry, dest_path);
}

bool epub_resolve_href(const char *opf_dir, const char *href,
                       char *out_path, size_t out_size)
{
    if (!opf_dir || !href || !out_path) return false;

    if (href[0] == '/') {
        return false;
    }

    char base[EPUB_PATH_MAX];
    if (opf_dir && opf_dir[0]) {
        snprintf(base, sizeof(base), "%s/", opf_dir);
    } else {
        base[0] = '\0';
    }

    char resolved[EPUB_PATH_MAX];
    const char *src = href;
    char *dst = resolved;

    while (*src) {
        if (src[0] == '.' && src[1] == '.' && (src[2] == '/' || src[2] == '\0')) {
            char *last_slash = strrchr(resolved, '/');
            if (last_slash) *last_slash = '\0';
            src += (src[2] == '/') ? 3 : 2;
        } else if (src[0] == '.' && (src[1] == '/' || src[1] == '\0')) {
            src += (src[1] == '/') ? 2 : 1;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';

    snprintf(out_path, out_size, "%s%s", base, resolved);
    return true;
}