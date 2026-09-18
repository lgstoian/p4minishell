/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file editor_spell.c
 * @brief Offline spellchecker core (see editor_spell.h).
 *
 * Memory model: one PSRAM pool holds the whole wordlist as NUL-separated,
 * lower-cased words; a second PSRAM array holds pointers into the pool.
 * Lookups binary-search the sorted pointer array. Everything is bounded by
 * P4_CONFIG_SPELL_* so a corrupt/huge file cannot exhaust memory.
 */

#include "editor_spell.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage.h"
#include "shell.h"
#include "p4minishell_config.h"
#include "esp_heap_caps.h"
#include "bsp/esp-bsp.h"

static char *s_pool;
static char **s_index;
static size_t s_count;
static bool s_ready;

/* qsort comparator: both arguments point at `char *` elements. */
static int spell_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* bsearch comparator: the key is the search string itself (`char *`), while
 * each element is a `char **`. */
static int spell_cmp_key(const void *key, const void *elem)
{
    return strcmp((const char *)key, *(const char *const *)elem);
}

bool editor_spell_ready(void)
{
    return s_ready && s_count > 0;
}

size_t editor_spell_word_count(void)
{
    return s_ready ? s_count : 0;
}

void editor_spell_unload(void)
{
    heap_caps_free(s_pool);
    heap_caps_free(s_index);
    s_pool = NULL;
    s_index = NULL;
    s_count = 0;
    s_ready = false;
}

bool editor_spell_load(const char *name)
{
    char rel[P4_CONFIG_SD_PATH_BYTES];
    char resolved[P4_CONFIG_SD_PATH_BYTES];
    shell_sd_session_t session;
    FILE *file;
    char *pool;
    char **index;
    uint8_t *bounce;
    size_t got = 0;
    size_t words = 0;
    size_t i;

    if (name == NULL || name[0] == '\0') {
        name = P4_CONFIG_SPELL_DICT_NAME;
    }
    snprintf(rel, sizeof(rel), "%s/%s.words",
             P4_CONFIG_SPELL_DICT_DIR_NAME, name);
    if (shell_fs_resolve_path(rel, resolved, sizeof(resolved)) != ESP_OK) {
        return false;
    }
    if (shell_sd_begin(&session) != ESP_OK) {
        return false;
    }
    file = fopen(resolved, "rb");
    if (file == NULL) {
        shell_sd_end(&session, "spell");
        return false;
    }
    pool = heap_caps_malloc(P4_CONFIG_SPELL_MAX_BYTES + 1,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pool == NULL) {
        pool = malloc(P4_CONFIG_SPELL_MAX_BYTES + 1);
    }
    /* SD/FATFS reads go through DMA: the bounce buffer MUST be internal,
     * DMA-capable memory. PSRAM is NOT MALLOC_CAP_DMA on this P4 build, so
     * reading straight into the PSRAM pool crashes (same rule the editor's
     * SD load/save follow). Read in chunks and copy into the pool. */
    bounce = heap_caps_malloc(P4_CONFIG_FILE_IO_BUFFER_BYTES,
                              MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (pool == NULL || bounce == NULL) {
        fclose(file);
        shell_sd_end(&session, "spell");
        heap_caps_free(pool);
        heap_caps_free(bounce);
        return false;
    }
    while (got < P4_CONFIG_SPELL_MAX_BYTES) {
        size_t want = P4_CONFIG_SPELL_MAX_BYTES - got;
        size_t r;
        if (want > P4_CONFIG_FILE_IO_BUFFER_BYTES) {
            want = P4_CONFIG_FILE_IO_BUFFER_BYTES;
        }
        r = fread(bounce, 1, want, file);
        if (r == 0) {
            break;
        }
        memcpy(pool + got, bounce, r);
        got += r;
    }
    heap_caps_free(bounce);
    fclose(file);
    pool[got] = '\0';
    shell_sd_end(&session, "spell");

    /* Split lines in place: NUL-terminate each word, lower-case it. */
    {
        size_t start = 0;
        for (i = 0; i <= got; i++) {
            char c = pool[i];
            if (c == '\n' || c == '\r' || c == '\0') {
                pool[i] = '\0';
                if (i > start) {
                    size_t j;
                    for (j = start; j < i; j++) {
                        pool[j] = (char)tolower((unsigned char)pool[j]);
                    }
                    if (words < P4_CONFIG_SPELL_MAX_WORDS) {
                        /* Index rebuilt below; just count here. */
                        words++;
                    }
                }
                start = i + 1;
            }
        }
    }
    if (words == 0) {
        heap_caps_free(pool);
        return false;
    }
    index = heap_caps_malloc((words + 1) * sizeof(*index),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (index == NULL) {
        index = malloc((words + 1) * sizeof(*index));
    }
    if (index == NULL) {
        heap_caps_free(pool);
        return false;
    }
    {
        size_t n = 0;
        size_t start = 0;
        for (i = 0; i <= got && n < words; i++) {
            if (pool[i] == '\0') {
                if (i > start) {
                    index[n++] = pool + start;
                }
                start = i + 1;
            }
        }
        words = n;
    }
    qsort(index, words, sizeof(*index), spell_cmp);

    /* Swap in (release the previous list only after the new one is ready). */
    heap_caps_free(s_pool);
    heap_caps_free(s_index);
    s_pool = pool;
    s_index = index;
    s_count = words;
    s_ready = true;
    return true;
}

bool editor_spell_ok(const char *word, size_t len)
{
    char lower[P4_CONFIG_SPELL_WORD_MAX];
    size_t i;

    if (!editor_spell_ready()) {
        return true; /* No dictionary: nothing is ever flagged. */
    }
    if (word == NULL || len < 2 || len >= sizeof(lower)) {
        return true; /* Too short to check or longer than any entry. */
    }
    for (i = 0; i < len; i++) {
        lower[i] = (char)tolower((unsigned char)word[i]);
    }
    lower[len] = '\0';
    return bsearch(lower, s_index, s_count, sizeof(*s_index),
                   spell_cmp_key) != NULL;
}
