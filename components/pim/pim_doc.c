/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file pim_doc.c
 * @brief Shared PSRAM render buffer for PIM interchange output.
 *
 * Moved out of components/command/export_commands.c: the single growable
 * document behind every `export` rendering path and every future `pim`
 * payload. Seed 4 KB PSRAM, doubles on growth, hard-capped (overflow is
 * sticky and every writer no-ops once set, so callers check once at the
 * end). Freed with heap_caps_free (pim_doc_free).
 */

#include "pim.h"
#include "p4heap.h"
#include "p4minishell_config.h"
#include "esp_heap_caps.h"

#include <string.h>

#ifndef P4_CONFIG_DB_EXPORT_MAX_BYTES
#define P4_CONFIG_DB_EXPORT_MAX_BYTES (256 * 1024)
#endif

void pim_doc_init(pim_doc_t *doc)
{
    doc->data = p4heap_alloc_psram(4096);
    doc->used = 0;
    doc->cap = (doc->data != NULL) ? 4096 : 0;
    doc->overflow = (doc->data == NULL);
    if (doc->data != NULL) {
        doc->data[0] = '\0';
    }
}

void pim_doc_free(pim_doc_t *doc)
{
    if (doc->data != NULL) {
        heap_caps_free(doc->data);
        doc->data = NULL;
    }
    doc->used = 0;
    doc->cap = 0;
}

void pim_doc_write(pim_doc_t *doc, const char *text, size_t len)
{
    if (doc->overflow || doc->data == NULL) {
        doc->overflow = true;
        return;
    }
    if (doc->used + len + 1 > (size_t)P4_CONFIG_DB_EXPORT_MAX_BYTES) {
        doc->overflow = true;
        return;
    }
    if (doc->used + len + 1 > doc->cap) {
        size_t grown = doc->cap * 2;
        char *grown_data;
        while (grown < doc->used + len + 1) {
            grown *= 2;
        }
        if (grown > (size_t)P4_CONFIG_DB_EXPORT_MAX_BYTES) {
            grown = P4_CONFIG_DB_EXPORT_MAX_BYTES;
        }
        grown_data = p4heap_realloc_psram(doc->data, grown);
        if (grown_data == NULL) {
            doc->overflow = true;
            return;
        }
        doc->data = grown_data;
        doc->cap = grown;
    }
    memcpy(doc->data + doc->used, text, len);
    doc->used += len;
    doc->data[doc->used] = '\0';
}

void pim_doc_text(pim_doc_t *doc, const char *text)
{
    pim_doc_write(doc, text, strlen(text));
}

void pim_doc_ical_text(pim_doc_t *doc, const char *text)
{
    for (const char *p = text; *p != '\0'; p++) {
        switch (*p) {
        case '\\':
            pim_doc_text(doc, "\\\\");
            break;
        case '\n':
            pim_doc_text(doc, "\\n");
            break;
        case '\r':
            break;
        case ',':
            pim_doc_text(doc, "\\,");
            break;
        case ';':
            pim_doc_text(doc, "\\;");
            break;
        default:
            pim_doc_write(doc, p, 1);
            break;
        }
    }
}

void pim_doc_ical_prop(pim_doc_t *doc, const char *prop, const char *text)
{
    pim_doc_text(doc, prop);
    pim_doc_text(doc, ":");
    pim_doc_ical_text(doc, text);
    pim_doc_text(doc, "\r\n");
}

void pim_copy_trunc(char *dst, size_t dst_size, const char *src)
{
    size_t n;

    if (dst == NULL || dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n > dst_size - 1) {
        n = dst_size - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}
