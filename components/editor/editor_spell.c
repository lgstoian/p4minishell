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

/* Length-unlimited lookup key: the token bytes plus their length (the
 * token is not NUL-terminated, so the comparator walks both sides). */
typedef struct {
    const char *s;
    size_t len;
} spell_lookup_t;

/* Compare a length-delimited token (ASCII case-folded on the fly) against
 * one lower-cased index entry. Reads neither side past its end. */
static int spell_cmp_token(const void *keyp, const void *elemp)
{
    const spell_lookup_t *k = (const spell_lookup_t *)keyp;
    const char *e = *(const char *const *)elemp;
    size_t i = 0;

    for (;;) {
        bool k_end = (i >= k->len);
        bool e_end = (e[i] == '\0');
        if (k_end && e_end) {
            return 0;
        }
        if (k_end) {
            return -1;
        }
        if (e_end) {
            return 1;
        }
        {
            int kd = tolower((unsigned char)k->s[i]);
            int ed = (unsigned char)e[i]; /* Entries are stored lower-cased. */
            if (kd != ed) {
                return kd - ed;
            }
        }
        i++;
    }
}

bool editor_spell_ok(const char *word, size_t len)
{
    spell_lookup_t key;

    if (!editor_spell_ready()) {
        return true; /* No dictionary: nothing is ever flagged. */
    }
    if (word == NULL || len == 0) {
        return true;
    }
    key.s = word;
    key.len = len;
    return bsearch(&key, s_index, s_count, sizeof(*s_index),
                   spell_cmp_token) != NULL;
}

size_t editor_spell_utf8(const char *s, size_t avail, unsigned long *cp_out)
{
    unsigned char lead;
    size_t len;
    unsigned long cp;
    size_t i;

    if (s == NULL || avail == 0 || s[0] == '\0') {
        return 0;
    }
    lead = (unsigned char)s[0];
    if (lead < 0x80) {
        if (cp_out != NULL) {
            *cp_out = lead;
        }
        return 1;
    }
    if ((lead & 0xE0) == 0xC0) {
        len = 2;
        cp = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
        len = 3;
        cp = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
        len = 4;
        cp = lead & 0x07;
    } else {
        return 0;
    }
    if (len > avail) {
        return 0;
    }
    for (i = 1; i < len; i++) {
        unsigned char cont = (unsigned char)s[i];
        if ((cont & 0xC0) != 0x80) {
            return 0;
        }
        cp = (cp << 6) | (cont & 0x3F);
    }
    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
        (len == 4 && cp < 0x10000) || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF)) {
        return 0;
    }
    if (cp_out != NULL) {
        *cp_out = cp;
    }
    return len;
}

bool editor_spell_is_cjk(unsigned long cp)
{
    return (cp >= 0x1100 && cp <= 0x115F) || /* Hangul Jamo */
           (cp >= 0x2E80 && cp <= 0x303E) || /* CJK roots/punctuation */
           (cp >= 0x3041 && cp <= 0x33FF) || /* Hiragana/Katakana/CJK compat */
           (cp >= 0x3400 && cp <= 0x4DBF) || /* Ext A */
           (cp >= 0x4E00 && cp <= 0x9FFF) || /* Unified ideographs */
           (cp >= 0xA000 && cp <= 0xA4CF) || /* Yi */
           (cp >= 0xAC00 && cp <= 0xD7AF) || /* Hangul syllables */
           (cp >= 0xF900 && cp <= 0xFAFF) || /* Compat ideographs */
           (cp >= 0xFE30 && cp <= 0xFE4F) || /* CJK compat forms */
           (cp >= 0xFF00 && cp <= 0xFF60) || /* Fullwidth forms */
           (cp >= 0x20000 && cp <= 0x3FFFD); /* Ext B and beyond */
}

bool editor_spell_is_letter(unsigned long cp)
{
    if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) {
        return true;
    }
    if ((cp >= 0xC0 && cp <= 0xD6) || (cp >= 0xD8 && cp <= 0xF6) ||
        (cp >= 0xF8 && cp <= 0xFF)) {
        return true; /* Latin-1 supplement letters (excluding x/divide). */
    }
    if ((cp >= 0x100 && cp <= 0x24F) || /* Latin Extended-A/B */
        (cp >= 0x1E00 && cp <= 0x1EFF) || /* Latin Extended Additional */
        (cp >= 0x370 && cp <= 0x3FF) || /* Greek */
        (cp >= 0x400 && cp <= 0x4FF) || /* Cyrillic */
        (cp >= 0x300 && cp <= 0x36F)) { /* Combining marks attach. */
        return true;
    }
    return editor_spell_is_cjk(cp);
}

bool editor_spell_is_joiner(unsigned long cp)
{
    return cp == '\'' || cp == '-' || cp == 0x2019;
}

bool editor_spell_token_ok(const char *word, size_t len)
{
    size_t i = 0;
    bool has_cjk = false;
    bool has_nonen = false;
    bool has_digit = false;
    bool has_score = false;
    bool checked = false;

    if (!editor_spell_ready()) {
        return true; /* No dictionary: nothing is ever flagged. */
    }
    if (word == NULL || len == 0) {
        return true;
    }
    while (i < len) {
        unsigned long cp = 0;
        size_t step = editor_spell_utf8(word + i, len - i, &cp);
        if (step == 0) {
            i++;
            continue;
        }
        if (editor_spell_is_cjk(cp)) {
            has_cjk = true;
        } else if (cp >= 0x80) {
            has_nonen = true;
        } else if (cp >= '0' && cp <= '9') {
            has_digit = true;
        } else if (cp == '_') {
            has_score = true;
        }
        i += step;
    }
    /* The wordlist cannot cover these: skip rather than split or flag. */
    if (has_cjk || has_digit || has_score || has_nonen) {
        return true;
    }
    if (editor_spell_ok(word, len)) {
        return true;
    }
    /* Contractions/hyphenates: every letter part of length >= 2 must hit
     * (shorter parts like the `t` in `don't` carry no meaning alone). */
    i = 0;
    while (i < len) {
        size_t a = i;
        while (i < len && word[i] != '\'' && word[i] != '-') {
            i++;
        }
        if (i > a) {
            if (i - a >= 2) {
                checked = true;
                if (!editor_spell_ok(word + a, i - a)) {
                    return false;
                }
            }
        } else {
            i++;
        }
    }
    return checked;
}
