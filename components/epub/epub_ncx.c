/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file epub_ncx.c
 * @brief NCX (Navigation Control XML) parser for EPUB2.
 *
 * Parses the Navigation Control XML for the table of contents. Streaming
 * Expat callbacks, matching epub.c's OPF parsing, so there is one XML
 * approach across the component.
 */

#include "epub.h"
#include "p4minishell_config.h"
#include <string.h>
#include <stdlib.h>
#include <esp_log.h>
#include <expat.h>


/* ------------------------------------------------------------------------- */
/* NCX XML parser state                                                      */
/* ------------------------------------------------------------------------- */

typedef struct {
    epub_toc_entry_t *toc;
    int max_entries;
    int *out_count;
    int depth;                 /* navPoint nesting depth (0 = top level) */
    bool in_nav_map;
    bool in_nav_label;
    bool in_text;
    epub_toc_entry_t *current; /* navPoint being filled, or NULL */
} ncx_parser_t;

/** Append @p s to @p dst if it fits, else return. */
static void ncx_append(char *dst, size_t cap, const char *s, size_t n)
{
    size_t used = strlen(dst);

    if (used >= cap - 1) {
        return;
    }
    if (n > cap - 1 - used) {
        n = cap - 1 - used;
    }
    memcpy(dst + used, s, n);
    dst[used + n] = '\0';
}

static void ncx_start(void *user_data, const char *name, const char **atts)
{
    ncx_parser_t *p = (ncx_parser_t *)user_data;

    if (strcmp(name, "navMap") == 0) {
        p->in_nav_map = true;
    } else if (strcmp(name, "navPoint") == 0) {
        if (p->in_nav_map && p->out_count != NULL && *p->out_count < p->max_entries) {
            epub_toc_entry_t *entry = &p->toc[*p->out_count];

            memset(entry, 0, sizeof(*entry));
            entry->chapter_index = -1;
            entry->depth = p->depth;
            p->current = entry;
            (*p->out_count)++;
        } else {
            p->current = NULL;
        }
        p->depth++;
    } else if (strcmp(name, "navLabel") == 0) {
        p->in_nav_label = true;
    } else if (strcmp(name, "text") == 0) {
        p->in_text = p->in_nav_label;
    } else if (strcmp(name, "content") == 0) {
        if (p->current != NULL) {
            int i;

            for (i = 0; atts != NULL && atts[i] != NULL; i += 2) {
                if (strcmp(atts[i], "src") == 0) {
                    snprintf(p->current->href, sizeof(p->current->href), "%s",
                             atts[i + 1]);
                }
            }
        }
    }
}

static void ncx_end(void *user_data, const char *name)
{
    ncx_parser_t *p = (ncx_parser_t *)user_data;

    if (strcmp(name, "text") == 0) {
        p->in_text = false;
    } else if (strcmp(name, "navLabel") == 0) {
        p->in_nav_label = false;
    } else if (strcmp(name, "navPoint") == 0) {
        if (p->depth > 0) {
            p->depth--;
        }
        /* An outer navPoint's label/content always precede its nested
         * navPoints, so there is nothing to resume here. */
        p->current = NULL;
    } else if (strcmp(name, "navMap") == 0) {
        p->in_nav_map = false;
    }
}

static void ncx_text(void *user_data, const char *s, int len)
{
    ncx_parser_t *p = (ncx_parser_t *)user_data;

    if (p->in_text && p->current != NULL && len > 0) {
        ncx_append(p->current->label, sizeof(p->current->label), s, (size_t)len);
    }
}

bool epub_parse_ncx(const char *xml, size_t len,
                    epub_toc_entry_t *out_toc,
                    int max_entries,
                    int *out_count)
{
    XML_Parser parser;
    ncx_parser_t state;
    bool ok;

    if (xml == NULL || out_toc == NULL || out_count == NULL || max_entries <= 0) {
        return false;
    }

    memset(&state, 0, sizeof(state));
    state.toc = out_toc;
    state.max_entries = max_entries;
    state.out_count = out_count;
    *out_count = 0;

    parser = XML_ParserCreate("UTF-8");
    if (parser == NULL) {
        return false;
    }
    XML_SetUserData(parser, &state);
    XML_SetElementHandler(parser, ncx_start, ncx_end);
    XML_SetCharacterDataHandler(parser, ncx_text);

    ok = XML_Parse(parser, xml, (int)len, 1) != 0;
    if (!ok) {
        ESP_LOGW("epub_ncx", "NCX parse error: %s at line %lu",
                 XML_ErrorString(XML_GetErrorCode(parser)),
                 XML_GetCurrentLineNumber(parser));
    }
    XML_ParserFree(parser);
    return ok;
}
