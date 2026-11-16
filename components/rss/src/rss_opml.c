/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file rss_opml.c
 * @brief OPML 2.0 interchange for the RSS subscription store.
 *
 * OPML is the interchange format; `sd:/RSS/SUBS.INI` stays the native store.
 * Export writes standard attributes (`text`, `title`, `type`, `xmlUrl`) plus
 * the `p4*` extensions that carry the subscription metadata (auth, validators,
 * interval, enabled) so a round-trip loses nothing. Import reads the standard
 * attributes and honours the `p4*` ones when present.
 *
 * XML parsing uses expat, the same parser components/epub and rss_parser use;
 * there is no second XML implementation in the firmware.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "expat.h"

#include "rss_store.h"

#define OPML_MAX_ATTR_BYTES      512

/* ========================================================================
 * EXPORT
 * ======================================================================== */

/** Write one escaped XML attribute value ("&", "<", ">", '"' and '\''). */
static void opml_write_escaped(FILE *file, const char *value)
{
    for (const char *p = value; *p != '\0'; p++) {
        switch (*p) {
        case '&':  fputs("&amp;", file);  break;
        case '<':  fputs("&lt;", file);   break;
        case '>':  fputs("&gt;", file);   break;
        case '"':  fputs("&quot;", file); break;
        case '\'': fputs("&apos;", file); break;
        default:   fputc(*p, file);       break;
        }
    }
}

void rss_opml_write(FILE *file, const rss_subscription_t *subs, int count)
{
    if (file == NULL || subs == NULL) {
        return;
    }

    fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n", file);
    fputs("<opml version=\"2.0\">\n", file);
    fputs("  <head>\n", file);
    fputs("    <title>P4MiniShell RSS feeds</title>\n", file);
    fputs("  </head>\n", file);
    fputs("  <body>\n", file);

    for (int i = 0; i < count; i++) {
        const rss_subscription_t *sub = &subs[i];

        if (!rss_store_name_ok(sub->name)) {
            continue;
        }
        fputs("    <outline text=\"", file);
        opml_write_escaped(file, sub->name);
        fputs("\" title=\"", file);
        opml_write_escaped(file, sub->title[0] != '\0' ? sub->title : sub->name);
        fputs("\" type=\"rss\" xmlUrl=\"", file);
        opml_write_escaped(file, sub->url);
        fputs("\" p4AuthType=\"", file);
        fprintf(file, "%d", (int)sub->auth_type);
        fputs("\" p4User=\"", file);
        opml_write_escaped(file, sub->auth_user);
        fputs("\" p4Token=\"", file);
        opml_write_escaped(file, sub->auth_token);
        fputs("\" p4Etag=\"", file);
        opml_write_escaped(file, sub->etag);
        fputs("\" p4Modified=\"", file);
        opml_write_escaped(file, sub->last_modified);
        fprintf(file, "\" p4Interval=\"%d\" p4Enabled=\"%d\" />\n",
                sub->interval_min, sub->enabled ? 1 : 0);
    }

    fputs("  </body>\n", file);
    fputs("</opml>\n", file);
}

/* ========================================================================
 * IMPORT
 * ======================================================================== */

typedef struct {
    rss_subscription_t *out;
    int max;
    int count;
} opml_ctx_t;

/** Force a candidate name into the store's name charset ([A-Za-z0-9_-],
 *  non-empty, under the capacity). Invalid characters become '_'. */
static void opml_sanitize_name(const char *raw, char *out, size_t out_size)
{
    size_t i;

    if (raw == NULL || out == NULL || out_size == 0) {
        return;
    }
    for (i = 0; i < out_size - 1 && raw[i] != '\0'; i++) {
        char c = raw[i];

        if (isalnum((unsigned char)c) || c == '_' || c == '-') {
            out[i] = c;
        } else {
            out[i] = '_';
        }
    }
    out[i] = '\0';
    if (out[0] == '\0') {
        strncpy(out, "feed", out_size - 1);
        out[out_size - 1] = '\0';
    }
}

/** Derive a name from a feed URL when the OPML entry has no usable text. */
static void opml_name_from_url(const char *url, char *out, size_t out_size)
{
    const char *cursor = url;
    char raw[OPML_MAX_ATTR_BYTES];

    if (url == NULL || out == NULL || out_size == 0) {
        return;
    }
    if (strncasecmp(url, "http://", 7) == 0) {
        cursor = url + 7;
    } else if (strncasecmp(url, "https://", 8) == 0) {
        cursor = url + 8;
    }
    strncpy(raw, cursor, sizeof(raw) - 1);
    raw[sizeof(raw) - 1] = '\0';
    {
        char *slash = strchr(raw, '/');

        if (slash != NULL) {
            *slash = '\0';
        }
    }
    opml_sanitize_name(raw, out, out_size);
}

/** Read a `name` attribute by case-insensitive match. */
static const char *opml_attr(const char **atts, const char *wanted)
{
    for (int i = 0; atts[i] != NULL; i += 2) {
        if (atts[i + 1] != NULL && strcasecmp(atts[i], wanted) == 0) {
            return atts[i + 1];
        }
    }
    return NULL;
}

static void XMLCALL opml_start_element(void *user_data, const char *name,
                                       const char **atts)
{
    opml_ctx_t *ctx = (opml_ctx_t *)user_data;
    const char *xml_url;
    const char *text;
    const char *value;
    rss_subscription_t parsed;
    char candidate[P4_CONFIG_RSS_SUB_NAME_BYTES];

    /* Only `<outline>` carries feeds; `<body>`/`<head>` are structure. */
    if (strcasecmp(name, "outline") != 0) {
        return;
    }
    xml_url = opml_attr(atts, "xmlUrl");
    if (xml_url == NULL || xml_url[0] == '\0') {
        return;                       /* a folder or a text-only entry */
    }
    if (ctx->count >= ctx->max) {
        return;
    }

    text = opml_attr(atts, "text");
    if (text == NULL || text[0] == '\0') {
        text = opml_attr(atts, "title");
    }
    if (text != NULL && text[0] != '\0') {
        opml_sanitize_name(text, candidate, sizeof(candidate));
    } else {
        opml_name_from_url(xml_url, candidate, sizeof(candidate));
    }

    memset(&parsed, 0, sizeof(parsed));
    strncpy(parsed.name, candidate, sizeof(parsed.name) - 1);
    strncpy(parsed.url, xml_url, sizeof(parsed.url) - 1);
    parsed.enabled = true;
    parsed.interval_min = 0;

    value = opml_attr(atts, "p4AuthType");
    if (value != NULL) {
        parsed.auth_type = (rss_auth_type_t)atoi(value);
        if (parsed.auth_type < RSS_AUTH_NONE || parsed.auth_type > RSS_AUTH_BEARER) {
            parsed.auth_type = RSS_AUTH_NONE;
        }
    }
    value = opml_attr(atts, "p4User");
    if (value != NULL) {
        strncpy(parsed.auth_user, value, sizeof(parsed.auth_user) - 1);
    }
    value = opml_attr(atts, "p4Token");
    if (value != NULL) {
        strncpy(parsed.auth_token, value, sizeof(parsed.auth_token) - 1);
    }
    value = opml_attr(atts, "p4Etag");
    if (value != NULL) {
        strncpy(parsed.etag, value, sizeof(parsed.etag) - 1);
    }
    value = opml_attr(atts, "p4Modified");
    if (value != NULL) {
        strncpy(parsed.last_modified, value, sizeof(parsed.last_modified) - 1);
    }
    value = opml_attr(atts, "p4Interval");
    if (value != NULL) {
        parsed.interval_min = atoi(value);
    }
    value = opml_attr(atts, "p4Enabled");
    if (value != NULL) {
        parsed.enabled = (atoi(value) != 0);
    }

    /* Existing names win: an import never clobbers a configured feed. */
    if (!rss_store_name_ok(parsed.name) ||
        rss_store_find(parsed.name) >= 0) {
        return;
    }
    ctx->out[ctx->count++] = parsed;
}

esp_err_t rss_opml_parse(const char *text, size_t len,
                         rss_subscription_t *out, int max, int *out_count)
{
    XML_Parser parser;
    opml_ctx_t ctx;
    enum XML_Status status;

    if (text == NULL || out == NULL || max <= 0 || out_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ctx.out = out;
    ctx.max = max;
    ctx.count = 0;

    parser = XML_ParserCreate(NULL);
    if (parser == NULL) {
        return ESP_ERR_NO_MEM;
    }
    XML_SetUserData(parser, &ctx);
    XML_SetElementHandler(parser, opml_start_element, NULL);

    status = XML_Parse(parser, text, (int)len, 1);
    XML_ParserFree(parser);
    if (status == XML_STATUS_ERROR) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = ctx.count;
    return ESP_OK;
}
