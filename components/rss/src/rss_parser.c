/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file rss_parser.c
 * @brief RSS 2.0 / Atom 1.0 / JSON Feed parsing (see rss.h).
 *
 * Reuse, not reimplementation:
 *   - XML streams through Expat, the same parser components/epub uses.
 *   - JSON Feed streams through components/json's json_walk(), the firmware's
 *     single JSON reader. There is no second JSON or XML scanner here.
 *
 * Parsing is pure (no I/O), so it is unit-testable with literal documents.
 */

#include "rss.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "expat.h"
#include "json.h"

#define RSS_LOG_TAG "rss"

/* ------------------------------------------------------------------------
 * Item array growth
 * ---------------------------------------------------------------------- */

/** Ensure room for one more item. Returns the slot or NULL (capacity/full). */
static rss_item_t *rss_feed_add_item(rss_feed_t *feed)
{
    int want;
    rss_item_t *grown;

    if (feed->item_count >= P4_CONFIG_RSS_ITEM_CAPACITY) {
        return NULL;
    }
    if (feed->item_count == feed->item_capacity) {
        want = (feed->item_capacity == 0) ? 16 : feed->item_capacity * 2;
        if (want > P4_CONFIG_RSS_ITEM_CAPACITY) {
            want = P4_CONFIG_RSS_ITEM_CAPACITY;
        }
        grown = heap_caps_realloc(feed->items, (size_t)want * sizeof(rss_item_t),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (grown == NULL) {
            return NULL;
        }
        feed->items = grown;
        feed->item_capacity = want;
    }
    memset(&feed->items[feed->item_count], 0, sizeof(rss_item_t));
    return &feed->items[feed->item_count];
}

static void rss_field_set(char *dst, size_t cap, const char *text, size_t len)
{
    if (len >= cap) {
        len = cap - 1;
    }
    memcpy(dst, text, len);
    dst[len] = '\0';
}

void rss_feed_free(rss_feed_t *feed)
{
    if (feed == NULL) {
        return;
    }
    if (feed->items != NULL) {
        heap_caps_free(feed->items);
    }
    memset(feed, 0, sizeof(*feed));
}

const char *rss_format_name(rss_format_t format)
{
    switch (format) {
    case RSS_FORMAT_RSS_2_0:   return "RSS";
    case RSS_FORMAT_ATOM_1_0:  return "Atom";
    case RSS_FORMAT_JSON_FEED: return "JSON Feed";
    default:                   return "unknown";
    }
}

/* ------------------------------------------------------------------------
 * XML (RSS 2.0 / Atom 1.0) via Expat
 * ---------------------------------------------------------------------- */

typedef struct {
    rss_feed_t *feed;
    rss_item_t *item;      /**< item being filled, NULL at feed level */
    bool in_item;
    char *dst;             /**< element whose character data is captured */
    size_t dst_cap;
    size_t dst_len;
    bool categories_append;
} rss_xml_t;

/** Point the character-data sink at one field (cleared to empty). */
static void rss_xml_capture(rss_xml_t *x, char *dst, size_t cap, bool append)
{
    x->dst = dst;
    x->dst_cap = cap;
    if (!append) {
        dst[0] = '\0';
    }
    x->dst_len = append ? strlen(dst) : 0;
    x->categories_append = append;
}

/** True when the element belongs to the current item rather than the feed. */
static bool rss_xml_target_item(const rss_xml_t *x)
{
    return x->in_item && x->item != NULL;
}

static void rss_xml_start(void *ud, const char *name, const char **atts)
{
    rss_xml_t *x = (rss_xml_t *)ud;
    rss_feed_t *f = x->feed;
    bool item = rss_xml_target_item(x);

    if (strcmp(name, "rss") == 0 || strcmp(name, "rdf:RDF") == 0) {
        if (f->format == RSS_FORMAT_UNKNOWN) {
            f->format = RSS_FORMAT_RSS_2_0;
        }
        return;
    }
    if (strcmp(name, "feed") == 0) {
        f->format = RSS_FORMAT_ATOM_1_0;
        return;
    }
    if (strcmp(name, "item") == 0 || strcmp(name, "entry") == 0) {
        rss_item_t *slot = rss_feed_add_item(f);

        x->item = slot;
        x->in_item = true;
        if (slot != NULL) {
            f->item_count++;
        }
        return;
    }

    /* Atom carries the URL in a link element's href attribute, not as text. */
    if (strcmp(name, "link") == 0) {
        int i;

        for (i = 0; atts != NULL && atts[i] != NULL; i += 2) {
            if (strcmp(atts[i], "href") == 0) {
                if (item) {
                    rss_field_set(x->item->link, sizeof(x->item->link), atts[i + 1],
                                  strlen(atts[i + 1]));
                } else {
                    rss_field_set(f->link, sizeof(f->link), atts[i + 1], strlen(atts[i + 1]));
                }
                break;
            }
        }
        return;
    }

    if (strcmp(name, "title") == 0) {
        rss_xml_capture(x, item ? x->item->title : f->title,
                        item ? sizeof(x->item->title) : sizeof(f->title), false);
    } else if (strcmp(name, "description") == 0 || strcmp(name, "subtitle") == 0) {
        if (item) {
            rss_xml_capture(x, x->item->summary, sizeof(x->item->summary), false);
        } else {
            rss_xml_capture(x, f->description, sizeof(f->description), false);
        }
    } else if (strcmp(name, "summary") == 0) {
        if (item) {
            rss_xml_capture(x, x->item->summary, sizeof(x->item->summary), false);
        }
    } else if (strcmp(name, "content") == 0 || strcmp(name, "content:encoded") == 0 ||
               strcmp(name, "encoded") == 0) {
        if (item) {
            rss_xml_capture(x, x->item->content, sizeof(x->item->content), false);
        }
    } else if (strcmp(name, "guid") == 0 || strcmp(name, "id") == 0) {
        if (item) {
            rss_xml_capture(x, x->item->guid, sizeof(x->item->guid), false);
        }
    } else if (strcmp(name, "author") == 0 || strcmp(name, "name") == 0) {
        if (item) {
            rss_xml_capture(x, x->item->author, sizeof(x->item->author), false);
        }
    } else if (strcmp(name, "category") == 0 || strcmp(name, "tag") == 0) {
        if (item) {
            rss_xml_capture(x, x->item->categories, sizeof(x->item->categories), true);
        }
    } else if (strcmp(name, "pubDate") == 0 || strcmp(name, "published") == 0) {
        if (item) {
            rss_xml_capture(x, x->item->published, sizeof(x->item->published), false);
        }
    } else if (strcmp(name, "updated") == 0 || strcmp(name, "lastBuildDate") == 0) {
        if (item) {
            rss_xml_capture(x, x->item->published, sizeof(x->item->published), false);
        } else {
            rss_xml_capture(x, f->updated, sizeof(f->updated), false);
        }
    } else if (strcmp(name, "generator") == 0) {
        rss_xml_capture(x, f->generator, sizeof(f->generator), false);
    }
}

static void rss_xml_end(void *ud, const char *name)
{
    rss_xml_t *x = (rss_xml_t *)ud;

    if (strcmp(name, "item") == 0 || strcmp(name, "entry") == 0) {
        x->in_item = false;
        x->item = NULL;
        x->dst = NULL;
        return;
    }

    if (x->dst != NULL) {
        /* Categories were appended with their existing separator/flush
         * handled by chardata; everything else is already terminated. */
        x->dst[x->dst_len < x->dst_cap ? x->dst_len : x->dst_cap - 1] = '\0';
        x->dst = NULL;
    }
}

static void rss_xml_chardata(void *ud, const char *s, int len)
{
    rss_xml_t *x = (rss_xml_t *)ud;
    size_t n = (size_t)len;

    if (x->dst == NULL || n == 0) {
        return;
    }
    if (x->dst_len + n >= x->dst_cap) {
        n = (x->dst_cap > x->dst_len + 1) ? (x->dst_cap - x->dst_len - 1) : 0;
    }
    if (n > 0) {
        memcpy(x->dst + x->dst_len, s, n);
        x->dst_len += n;
    }
}

/* ------------------------------------------------------------------------
 * JSON Feed via components/json
 * ---------------------------------------------------------------------- */

typedef struct {
    rss_feed_t *feed;
    int current_item;   /**< index parsed from "items.N.*", or -1 */
} rss_json_t;

/** Copy a JSON string value (still escaped) into a bounded destination. */
static void rss_json_set(char *dst, size_t cap, const char *raw, size_t raw_len)
{
    json_unescape_span(raw, raw_len, dst, cap);
}

/** Parse the numeric segment of "items.N.<field>"; -1 when not an item path. */
static int rss_json_item_index(const char *path)
{
    const char *p;
    int idx = 0;
    bool digits = false;

    if (strncmp(path, "items.", 6) != 0) {
        return -1;
    }
    for (p = path + 6; *p != '\0' && *p != '.'; p++) {
        if (*p < '0' || *p > '9') {
            return -1;
        }
        idx = idx * 10 + (*p - '0');
        digits = true;
    }
    return digits ? idx : -1;
}

static bool rss_json_visit(const char *path, const char *value, size_t len, void *ctx)
{
    rss_json_t *jx = (rss_json_t *)ctx;
    rss_feed_t *f = jx->feed;
    int idx;
    const char *field;

    idx = rss_json_item_index(path);
    if (idx < 0) {
        /* Feed-level fields. */
        if (strcmp(path, "title") == 0) {
            rss_json_set(f->title, sizeof(f->title), value, len);
        } else if (strcmp(path, "home_page_url") == 0) {
            rss_json_set(f->link, sizeof(f->link), value, len);
        } else if (strcmp(path, "description") == 0) {
            rss_json_set(f->description, sizeof(f->description), value, len);
        } else if (strcmp(path, "version") == 0) {
            f->format = RSS_FORMAT_JSON_FEED;
        }
        return true;
    }

    /* Grow the item array up to the requested index. */
    while (f->item_count <= idx && f->item_count < P4_CONFIG_RSS_ITEM_CAPACITY) {
        if (rss_feed_add_item(f) == NULL) {
            return false;
        }
        f->item_count++;
    }
    if (idx >= f->item_count) {
        return true;   /* beyond capacity: ignore */
    }

    field = strchr(path + 6, '.');
    field = (field != NULL) ? field + 1 : "";
    {
        rss_item_t *it = &f->items[idx];

        if (strcmp(field, "id") == 0) {
            rss_json_set(it->guid, sizeof(it->guid), value, len);
        } else if (strcmp(field, "url") == 0) {
            rss_json_set(it->link, sizeof(it->link), value, len);
        } else if (strcmp(field, "title") == 0) {
            rss_json_set(it->title, sizeof(it->title), value, len);
        } else if (strcmp(field, "summary") == 0) {
            rss_json_set(it->summary, sizeof(it->summary), value, len);
        } else if (strcmp(field, "content_html") == 0 || strcmp(field, "content_text") == 0) {
            rss_json_set(it->content, sizeof(it->content), value, len);
        } else if (strcmp(field, "date_published") == 0) {
            rss_json_set(it->published, sizeof(it->published), value, len);
        } else if (strcmp(field, "name") == 0) {
            /* authors.N.name */
            rss_json_set(it->author, sizeof(it->author), value, len);
        } else if (strcmp(field, "tags") == 0) {
            rss_json_set(it->categories, sizeof(it->categories), value, len);
        }
    }
    return true;
}

/* ------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */

/** Looks like JSON when the first non-space byte is '{' or '['. */
static bool rss_looks_like_json(const char *data, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        char c = data[i];

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            continue;
        }
        return c == '{' || c == '[';
    }
    return false;
}

esp_err_t rss_parse(const char *data, size_t len, rss_feed_t *feed)
{
    if (data == NULL || len == 0 || feed == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(feed, 0, sizeof(*feed));

    if (rss_looks_like_json(data, len)) {
        rss_json_t jx = { .feed = feed, .current_item = -1 };
        char err[64];

        if (!json_walk(data, len, rss_json_visit, &jx, err, sizeof(err))) {
            return ESP_ERR_INVALID_ARG;
        }
        feed->format = RSS_FORMAT_JSON_FEED;
        return ESP_OK;
    }

    {
        XML_Parser parser = XML_ParserCreate("UTF-8");
        rss_xml_t x;
        bool ok;

        if (parser == NULL) {
            return ESP_ERR_NO_MEM;
        }
        memset(&x, 0, sizeof(x));
        x.feed = feed;
        XML_SetUserData(parser, &x);
        XML_SetElementHandler(parser, rss_xml_start, rss_xml_end);
        XML_SetCharacterDataHandler(parser, rss_xml_chardata);

        ok = XML_Parse(parser, data, (int)len, 1) != 0;
        XML_ParserFree(parser);

        if (!ok || feed->format == RSS_FORMAT_UNKNOWN) {
            rss_feed_free(feed);
            return ESP_ERR_INVALID_ARG;
        }
        return ESP_OK;
    }
}
