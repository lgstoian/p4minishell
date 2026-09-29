/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file pim_parse.c
 * @brief Shared vCard/iCalendar content-line plumbing.
 *
 * Moved out of components/command/import_commands.c: the single physical-
 * line reader, the unfolding pump (continuation lines carry a leading
 * SP/HTAB), and the pure "NAME;params:value" splitter behind both the vCard
 * and the iCalendar line handlers. Pump buffers are PSRAM.
 */

#include "pim.h"
#include "p4heap.h"
#include "p4minishell_config.h"
#include "esp_heap_caps.h"

#include <string.h>

#ifndef P4_CONFIG_TEXT_LINE_BYTES
#define P4_CONFIG_TEXT_LINE_BYTES 512
#endif

bool pim_next_line(FILE *file, char *line, size_t size)
{
    while (fgets(line, (int)size, file) != NULL) {
        const char *p = line;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            p++;
        }
        if (*p != '\0') {
            return true;
        }
    }
    return false;
}

/** Strip trailing CR/LF in place. */
static void pim_strip_eol(char *line)
{
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
        line[--len] = '\0';
    }
}

void pim_pump_unfolded(FILE *file, pim_line_emit_t emit, void *ctx)
{
    char *line = p4heap_alloc_psram(PIM_LINE_BYTES);
    char *unfolded = p4heap_alloc_psram(PIM_LINE_BYTES);

    if (line == NULL || unfolded == NULL) {
        if (line != NULL) {
            heap_caps_free(line);
        }
        if (unfolded != NULL) {
            heap_caps_free(unfolded);
        }
        return;
    }
    unfolded[0] = '\0';
    while (pim_next_line(file, line, PIM_LINE_BYTES)) {
        size_t ulen;
        pim_strip_eol(line);
        if ((line[0] == ' ' || line[0] == '\t') && unfolded[0] != '\0') {
            ulen = strlen(unfolded);
            if (ulen + strlen(line) < PIM_LINE_BYTES) {
                memmove(unfolded + ulen, line + 1, strlen(line));
            }
            continue;
        }
        if (unfolded[0] != '\0') {
            emit(unfolded, ctx);
        }
        snprintf(unfolded, PIM_LINE_BYTES, "%s", line);
    }
    if (unfolded[0] != '\0') {
        emit(unfolded, ctx);
    }
    heap_caps_free(line);
    heap_caps_free(unfolded);
}

bool pim_vcf_prop_split(const char *line, char *name_out, size_t name_size,
                        const char **value_out)
{
    const char *colon;
    const char *head_end;
    const char *name;
    const char *semi;
    const char *dot;
    size_t namelen;

    if (line == NULL || name_out == NULL || name_size == 0 || value_out == NULL) {
        return false;
    }
    colon = strchr(line, ':');
    if (colon == NULL || colon == line) {
        return false;
    }
    head_end = colon;
    /* The property name ends at the first ';' (parameters follow). */
    semi = memchr(line, ';', (size_t)(colon - line));
    if (semi != NULL) {
        head_end = semi;
    }
    name = line;
    namelen = (size_t)(head_end - line);
    /* Strip a group prefix ("item1.TEL" -> "TEL"). */
    dot = memchr(line, '.', namelen);
    if (dot != NULL) {
        name = dot + 1;
        namelen = (size_t)(head_end - name);
    }
    if (namelen == 0 || namelen + 1 > name_size) {
        return false;
    }
    memcpy(name_out, name, namelen);
    name_out[namelen] = '\0';
    *value_out = colon + 1;
    return true;
}
