/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file epub_nav.c
 * @brief EPUB3 nav.xhtml parser for the table of contents.
 *
 * Same Expat approach as the OPF/NCX readers: one XML parser for the whole
 * component. Entries are the `<a href="...">label</a>` items inside the
 * `<nav epub:type="toc">` element; nesting maps to `depth`, so `epub toc`
 * prints a readable tree for both EPUB2 (NCX) and EPUB3 (nav) books.
 */

#include "epub.h"
#include "p4minishell_config.h"
#include <string.h>
#include <stdlib.h>
#include <esp_log.h>
#include <expat.h>

typedef struct {
    epub_toc_entry_t *toc;
    int max_entries;
    int *out_count;
    int nav_depth;              /* >0 inside the TOC nav element */
    int list_depth;             /* <ol>/<ul> nesting inside it */
    bool in_toc_nav;
    bool in_anchor;
    epub_toc_entry_t *current;
} nav_parser_t;

static void nav_start(void *user, const char *name, const char **attrs)
{
    nav_parser_t *p = (nav_parser_t *)user;

    if (strcmp(name, "nav") == 0) {
        for (int i = 0; attrs[i] != NULL && attrs[i + 1] != NULL; i += 2) {
            if (strcmp(attrs[i], "epub:type") == 0 &&
                strstr(attrs[i + 1], "toc") != NULL) {
                p->in_toc_nav = true;
                p->nav_depth = 0;
                p->list_depth = 0;
                return;
            }
        }
        return;
    }
    if (!p->in_toc_nav) {
        return;
    }
    if (strcmp(name, "ol") == 0 || strcmp(name, "ul") == 0) {
        p->list_depth++;
        return;
    }
    if (strcmp(name, "a") == 0) {
        for (int i = 0; attrs[i] != NULL && attrs[i + 1] != NULL; i += 2) {
            if (strcmp(attrs[i], "href") != 0) {
                continue;
            }
            if (*p->out_count >= p->max_entries) {
                return;
            }
            p->current = &p->toc[*p->out_count];
            memset(p->current, 0, sizeof(*p->current));
            strlcpy(p->current->href, attrs[i + 1],
                    sizeof(p->current->href));
            p->current->depth = p->list_depth > 0 ? p->list_depth - 1 : 0;
            p->current->chapter_index = -1;
            p->in_anchor = true;
            return;
        }
    }
}

static void nav_end(void *user, const char *name)
{
    nav_parser_t *p = (nav_parser_t *)user;

    if (!p->in_toc_nav) {
        return;
    }
    if (strcmp(name, "a") == 0 && p->in_anchor) {
        p->in_anchor = false;
        if (p->current != NULL) {
            (*p->out_count)++;
            p->current = NULL;
        }
        return;
    }
    if (strcmp(name, "ol") == 0 || strcmp(name, "ul") == 0) {
        if (p->list_depth > 0) {
            p->list_depth--;
        }
        return;
    }
    if (strcmp(name, "nav") == 0) {
        p->in_toc_nav = false;
        p->in_anchor = false;
        p->current = NULL;
    }
}

static void nav_text(void *user, const char *text, int len)
{
    nav_parser_t *p = (nav_parser_t *)user;

    if (!p->in_anchor || p->current == NULL || len <= 0) {
        return;
    }
    size_t used = strlen(p->current->label);
    size_t cap = sizeof(p->current->label);

    if (used >= cap - 1) {
        return;
    }
    if ((size_t)len > cap - 1 - used) {
        len = (int)(cap - 1 - used);
    }
    memcpy(p->current->label + used, text, (size_t)len);
    p->current->label[used + (size_t)len] = '\0';
}

bool epub_parse_nav(const char *xml, size_t len, epub_toc_entry_t *out_toc,
                    int max_entries, int *out_count)
{
    nav_parser_t parser;
    XML_Parser expat;
    bool ok;

    if (xml == NULL || out_toc == NULL || out_count == NULL || len == 0) {
        return false;
    }
    memset(&parser, 0, sizeof(parser));
    parser.toc = out_toc;
    parser.max_entries = max_entries;
    parser.out_count = out_count;
    *out_count = 0;

    expat = XML_ParserCreate("UTF-8");
    if (expat == NULL) {
        return false;
    }
    XML_SetUserData(expat, &parser);
    XML_SetElementHandler(expat, nav_start, nav_end);
    XML_SetCharacterDataHandler(expat, nav_text);
    ok = XML_Parse(expat, xml, (int)len, 1) != XML_STATUS_ERROR;
    XML_ParserFree(expat);

    if (!ok) {
        *out_count = 0;
    }
    return *out_count > 0;
}
