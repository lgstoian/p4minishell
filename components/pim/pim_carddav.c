/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file pim_carddav.c
 * @brief Pure CardDAV helpers behind `pim net pull|push|sync`.
 *
 * Namespace-tolerant multistatus scanner, VCALENDAR/VCard splitting, UID
 * extraction, and URL joining. No network or SD access (the HTTP I/O lives
 * in the command layer on top of networking_http_request, and merging uses
 * the serial pim_vcf_merge_stream core), so everything here is unit-tested.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "pim.h"

/** Decode one XML entity at @p src into @p out (single char). Returns the
 *  source bytes consumed (including `&` and `;`), or 0 when not an entity. */
static size_t pim_carddav_entity(const char *src, char *out)
{
    static const struct {
        const char *name;
        char value;
    } entities[] = {
        { "amp;", '&' },
        { "lt;", '<' },
        { "gt;", '>' },
        { "quot;", '"' },
        { "apos;", '\'' },
    };
    size_t i;

    if (src[0] != '&') {
        return 0;
    }
    for (i = 0; i < sizeof(entities) / sizeof(entities[0]); i++) {
        size_t len = strlen(entities[i].name);

        if (strncmp(src + 1, entities[i].name, len) == 0) {
            *out = entities[i].value;
            return len + 1;
        }
    }
    return 0;
}

/** Copy @p len bytes from @p src to @p dst decoding XML entities. Always
 *  NUL-terminates (truncating when @p cap runs out). Returns bytes used. */
static size_t pim_carddav_decode_entities(const char *src, size_t len,
                                          char *dst, size_t cap)
{
    size_t used = 0;
    size_t i = 0;

    if (dst == NULL || cap == 0) {
        return 0;
    }
    while (i < len && used + 1 < cap) {
        char decoded = '\0';
        size_t consumed = pim_carddav_entity(src + i, &decoded);

        if (consumed > 0 && i + consumed <= len) {
            dst[used++] = decoded;
            i += consumed;
        } else {
            dst[used++] = src[i++];
        }
    }
    dst[used] = '\0';
    return used;
}

/** Match an XML opening tag at @p p whose local name is @p local, tolerating
 *  any namespace prefix (`<D:href>`, `<href>`). On match sets @p *content to
 *  the first byte after '>' and returns true. */
static bool pim_carddav_open_tag(const char *p, const char *local,
                                 const char **content)
{
    const char *name;
    size_t local_len = strlen(local);

    if (p[0] != '<' || p[1] == '/' || p[1] == '!' || p[1] == '?') {
        return false;
    }
    name = p + 1;
    /* Skip an optional namespace prefix. */
    {
        const char *colon = strchr(name, ':');
        const char *end = name;

        while (*end != '\0' && *end != '>' && *end != '/' && *end != ' ' &&
               *end != '\t' && *end != '\r' && *end != '\n') {
            end++;
        }
        if (colon != NULL && colon < end) {
            name = colon + 1;
        }
        if ((size_t)(end - name) != local_len || strncmp(name, local, local_len) != 0) {
            return false;
        }
        /* Attributes are not expected on href/getetag; require '>' or '/>'. */
        p = end;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '/') {
            return false;
        }
        if (*p != '>') {
            return false;
        }
        if (content != NULL) {
            *content = p + 1;
        }
        return true;
    }
}

/** Find the closing tag matching @p local at or after @p p (same
 *  prefix-tolerant rule). Returns the byte after the tag, or NULL. */
static const char *pim_carddav_close_tag(const char *p, const char *local)
{
    size_t local_len = strlen(local);

    while (*p != '\0') {
        if (p[0] == '<' && p[1] == '/') {
            const char *name = p + 2;
            const char *colon = strchr(name, ':');
            const char *end = name;

            while (*end != '\0' && *end != '>') {
                end++;
            }
            if (colon != NULL && colon < end) {
                name = colon + 1;
            }
            if ((size_t)(end - name) == local_len && strncmp(name, local, local_len) == 0) {
                return (*end == '>') ? end + 1 : NULL;
            }
            p = (*end == '\0') ? end : end + 1;
        } else {
            p++;
        }
    }
    return NULL;
}

int pim_carddav_scan_multistatus(const char *xml, size_t len,
                                 pim_carddav_response_cb_t on_response, void *ctx)
{
    const char *p;
    const char *end;
    int count = 0;

    if (on_response == NULL) {
        return -1;
    }
    if (xml == NULL || len == 0) {
        return 0;
    }
    end = xml + len;
    p = xml;
    while (p < end) {
        /* Next <response> block (any namespace prefix). */
        const char *content = NULL;

        while (p < end && !pim_carddav_open_tag(p, "response", &content)) {
            p++;
        }
        if (p >= end) {
            break;
        }
        {
            const char *block_end = pim_carddav_close_tag(content, "response");
            const char *q = content;
            const char *href_content = NULL;
            const char *href_end = NULL;
            const char *etag_content = NULL;
            const char *etag_end = NULL;
            const char *stop = (block_end != NULL) ? block_end : end;
            char href[512];
            char etag[128];

            while (q < stop && href_content == NULL) {
                if (pim_carddav_open_tag(q, "href", &href_content)) {
                    const char *c = href_content;

                    /* Content runs to the '<' that starts </href>. */
                    while (c < stop && !(c[0] == '<' && c[1] == '/')) {
                        c++;
                    }
                    href_end = c;
                    q = href_end;
                } else {
                    q++;
                }
            }
            q = content;
            while (q < stop && etag_content == NULL) {
                if (pim_carddav_open_tag(q, "getetag", &etag_content)) {
                    const char *c = etag_content;

                    while (c < stop && !(c[0] == '<' && c[1] == '/')) {
                        c++;
                    }
                    etag_end = c;
                    q = etag_end;
                } else {
                    q++;
                }
            }
            if (href_content != NULL && href_end > href_content) {
                size_t raw_len = (size_t)(href_end - href_content);

                if (raw_len >= sizeof(href)) {
                    raw_len = sizeof(href) - 1;
                }
                pim_carddav_decode_entities(href_content, raw_len, href, sizeof(href));
                etag[0] = '\0';
                if (etag_content != NULL && etag_end > etag_content) {
                    size_t etag_len = (size_t)(etag_end - etag_content);

                    if (etag_len >= sizeof(etag)) {
                        etag_len = sizeof(etag) - 1;
                    }
                    memcpy(etag, etag_content, etag_len);
                    etag[etag_len] = '\0';
                }
                on_response(href, etag[0] != '\0' ? etag : NULL, ctx);
                count++;
            }
            p = (block_end != NULL) ? block_end : end;
        }
    }
    return count;
}

int pim_carddav_split_cards(const char *text, size_t len,
                             pim_carddav_card_cb_t on_card, void *ctx)
{
    const char *p;
    const char *end;
    int count = 0;
    static const char begin_marker[] = "BEGIN:VCARD";
    static const char end_marker[] = "END:VCARD";

    if (on_card == NULL) {
        return -1;
    }
    if (text == NULL || len == 0) {
        return 0;
    }
    end = text + len;
    p = text;
    while (p < end) {
        const char *begin = NULL;
        const char *finish = NULL;
        const char *q = p;

        while (q + sizeof(begin_marker) - 1 <= end) {
            if (strncmp(q, begin_marker, sizeof(begin_marker) - 1) == 0) {
                begin = q;
                break;
            }
            q++;
        }
        if (begin == NULL) {
            break;
        }
        q = begin + sizeof(begin_marker) - 1;
        while (q + sizeof(end_marker) - 1 <= end) {
            if (strncmp(q, end_marker, sizeof(end_marker) - 1) == 0) {
                finish = q + sizeof(end_marker) - 1;
                while (finish < end && (*finish == '\r' || *finish == '\n')) {
                    finish++;
                }
                break;
            }
            q++;
        }
        if (finish == NULL) {
            break;
        }
        on_card(begin, (size_t)(finish - begin), ctx);
        count++;
        p = finish;
    }
    return count;
}

size_t pim_carddav_card_uid(const char *card, size_t len, char *uid_out, size_t uid_cap)
{
    const char *p;
    const char *end;

    if (uid_out == NULL || uid_cap == 0) {
        return 0;
    }
    uid_out[0] = '\0';
    if (card == NULL || len == 0) {
        return 0;
    }
    end = card + len;
    p = card;
    while (p < end) {
        const char *line_end = p;

        while (line_end < end && *line_end != '\r' && *line_end != '\n') {
            line_end++;
        }
        /* "UID" optionally followed by ";params" (vCard 4.0), then ':'. */
        if ((size_t)(line_end - p) > 4 &&
            (p[0] == 'U' || p[0] == 'u') &&
            (p[1] == 'I' || p[1] == 'i') &&
            (p[2] == 'D' || p[2] == 'd') &&
            (p[3] == ':' || p[3] == ';')) {
            const char *value = p + 3;

            if (*value == ';') {
                while (value < line_end && *value != ':') {
                    value++;
                }
            }
            if (value < line_end && *value == ':') {
                size_t vlen;

                value++;
                while (value < line_end && (*value == ' ' || *value == '\t')) {
                    value++;
                }
                vlen = (size_t)(line_end - value);
                while (vlen > 0 && (value[vlen - 1] == ' ' || value[vlen - 1] == '\t')) {
                    vlen--;
                }
                if (vlen > 0 && vlen < uid_cap) {
                    memcpy(uid_out, value, vlen);
                    uid_out[vlen] = '\0';
                    return vlen;
                }
                return 0;
            }
        }
        p = line_end;
        while (p < end && (*p == '\r' || *p == '\n')) {
            p++;
        }
    }
    return 0;
}

bool pim_carddav_join_url(const char *base, const char *href, char *out, size_t out_size)
{
    size_t base_len;
    size_t host_len;

    if (base == NULL || href == NULL || out == NULL || out_size == 0) {
        return false;
    }
    /* Absolute href wins as-is. */
    if (strncmp(href, "http://", 7) == 0 || strncmp(href, "https://", 8) == 0) {
        snprintf(out, out_size, "%s", href);
        return true;
    }
    /* Scheme + authority of the base. */
    {
        const char *scheme_end = strstr(base, "://");
        const char *slash;

        if (scheme_end == NULL) {
            return false;
        }
        slash = strchr(scheme_end + 3, '/');
        host_len = (slash != NULL) ? (size_t)(slash - base) : strlen(base);
    }
    base_len = strlen(base);
    while (base_len > 0 && base[base_len - 1] == '/') {
        base_len--;
    }
    if (href[0] == '/') {
        if (host_len + strlen(href) + 1 > out_size) {
            return false;
        }
        memcpy(out, base, host_len);
        snprintf(out + host_len, out_size - host_len, "%s", href);
        return true;
    }
    {
        /* Directory of the base (up to the last '/'; a trailing slash
         * means the last segment IS a directory and stays). */
        const char *last = NULL;
        const char *q = base;

        while (*q != '\0') {
            if (*q == '/') {
                last = q;
            }
            q++;
        }
        if (last != NULL) {
            size_t dir_len = (size_t)(last - base) + 1;

            if (dir_len + strlen(href) + 1 > out_size) {
                return false;
            }
            memcpy(out, base, dir_len);
            snprintf(out + dir_len, out_size - dir_len, "%s", href);
            return true;
        }
    }
    if (base_len + 1 + strlen(href) + 1 > out_size) {
        return false;
    }
    memcpy(out, base, base_len);
    out[base_len] = '/';
    snprintf(out + base_len + 1, out_size - base_len - 1, "%s", href);
    return true;
}

/** True when @p uid is safe to embed in a PUT filename (uuid-style). */
bool pim_carddav_uid_safe(const char *uid)
{
    const char *p;

    if (uid == NULL || *uid == '\0') {
        return false;
    }
    for (p = uid; *p != '\0'; p++) {
        char c = *p;

        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
              c == '@')) {
            return false;
        }
    }
    return true;
}
