/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file epub_export.c
 * @brief Markdown -> EPUB 3 packaging (see epub_export.h).
 *
 * One chapter, one manifest, one spine item. The chapter body comes from
 * `markdown_render_html_xhtml`, so the e-book shows exactly what the HTML
 * export and the on-device reader show - there is no second Markdown renderer.
 */

#include "epub_export.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "markdown.h"
#include "zip_writer.h"
#include "p4minishell_config.h"

/** Chapter body cap: the writerdeck export cap (256 KB) plus wrapper room. */
#define EPUB_EXPORT_BODY_BYTES (P4_CONFIG_MD_EXPORT_MAX_BYTES + 4096)

/** Escape @p text for XML text/attribute content (title only; the chapter
 *  body is already HTML-escaped by the Markdown writer). */
static void xml_escape(const char *text, char *out, size_t out_size)
{
    size_t used = 0;

    if (out_size == 0) {
        return;
    }
    for (; text != NULL && *text != '\0' && used + 6 < out_size; text++) {
        switch (*text) {
        case '&':
            memcpy(out + used, "&amp;", 5);
            used += 5;
            break;
        case '<':
            memcpy(out + used, "&lt;", 4);
            used += 4;
            break;
        case '>':
            memcpy(out + used, "&gt;", 4);
            used += 4;
            break;
        case '"':
            memcpy(out + used, "&quot;", 6);
            used += 6;
            break;
        default:
            out[used++] = *text;
            break;
        }
    }
    out[used] = '\0';
}

static esp_err_t add_entry(zip_writer_t *zip, const char *name,
                           const char *text)
{
    return zip_writer_add(zip, name, text, strlen(text));
}

esp_err_t epub_export_write(const char *path, const char *title,
                            const char *markdown)
{
    zip_writer_t *zip = NULL;
    char *body = NULL;
    char *chapter = NULL;
    char *opf = NULL;
    char *nav = NULL;
    char *ncx = NULL;
    char safe_title[192];
    char safe_name[192];
    const char *name;
    esp_err_t result;

    if (path == NULL || path[0] == '\0' || markdown == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (title == NULL || title[0] == '\0') {
        const char *slash = strrchr(path, '/');
        name = (slash != NULL) ? slash + 1 : path;
    } else {
        name = title;
    }
    xml_escape(name, safe_title, sizeof(safe_title));
    /* The identifier is derived from the (escaped) title, which keeps two
     * exports of different documents distinct without a clock. */
    snprintf(safe_name, sizeof(safe_name), "%s", safe_title);

    body = malloc(EPUB_EXPORT_BODY_BYTES);
    chapter = malloc(EPUB_EXPORT_BODY_BYTES);
    opf = malloc(4096);
    nav = malloc(2048);
    ncx = malloc(2048);
    if (body == NULL || chapter == NULL || opf == NULL || nav == NULL ||
        ncx == NULL) {
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    if (markdown_render_html_xhtml(markdown, body, EPUB_EXPORT_BODY_BYTES) >=
        EPUB_EXPORT_BODY_BYTES) {
        result = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    snprintf(chapter, EPUB_EXPORT_BODY_BYTES,
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<!DOCTYPE html>\n"
             "<html xmlns=\"http://www.w3.org/1999/xhtml\" lang=\"en\">\n"
             "<head>\n<meta charset=\"utf-8\"/>\n<title>%s</title>\n"
             "<style>body{font-family:serif;line-height:1.5;margin:1em}"
             "h1,h2,h3{line-height:1.2}pre{white-space:pre-wrap}"
             "table{border-collapse:collapse}td,th{border:1px solid #888;"
             "padding:2px 4px}</style>\n</head>\n<body>\n%s\n</body>\n"
             "</html>\n",
             safe_title, body);

    snprintf(opf, 4096,
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" "
             "unique-identifier=\"bookid\">\n"
             "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
             "<dc:identifier id=\"bookid\">urn:p4minishell:%s</dc:identifier>\n"
             "<dc:title>%s</dc:title>\n"
             "<dc:language>en</dc:language>\n"
             "<meta property=\"dcterms:modified\">2026-01-01T00:00:00Z</meta>\n"
             "</metadata>\n"
             "<manifest>\n"
             "<item id=\"nav\" href=\"nav.xhtml\" "
             "media-type=\"application/xhtml+xml\" properties=\"nav\"/>\n"
             "<item id=\"ncx\" href=\"toc.ncx\" "
             "media-type=\"application/x-dtbncx+xml\"/>\n"
             "<item id=\"chapter\" href=\"chapter.xhtml\" "
             "media-type=\"application/xhtml+xml\"/>\n"
             "</manifest>\n"
             "<spine toc=\"ncx\"><itemref idref=\"chapter\"/></spine>\n"
             "</package>\n",
             safe_name, safe_title);

    snprintf(nav, 2048,
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<!DOCTYPE html>\n"
             "<html xmlns=\"http://www.w3.org/1999/xhtml\" "
             "xmlns:epub=\"http://www.idpf.org/2007/ops\" lang=\"en\">\n"
             "<head><title>%s</title></head>\n"
             "<body><nav epub:type=\"toc\" id=\"toc\"><h1>%s</h1>\n"
             "<ol><li><a href=\"chapter.xhtml\">%s</a></li></ol></nav></body>\n"
             "</html>\n",
             safe_title, safe_title, safe_title);

    snprintf(ncx, 2048,
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" "
             "version=\"2005-1\">\n"
             "<head><meta name=\"dtb:uid\" content=\"urn:p4minishell:%s\"/>"
             "</head>\n"
             "<docTitle><text>%s</text></docTitle>\n"
             "<navMap><navPoint id=\"n1\" playOrder=\"1\">"
             "<navLabel><text>%s</text></navLabel>"
             "<content src=\"chapter.xhtml\"/></navPoint></navMap>\n"
             "</ncx>\n",
             safe_name, safe_title, safe_title);

    result = zip_writer_create(path, &zip);
    if (result != ESP_OK) {
        goto cleanup;
    }
    /* Order matters: `mimetype` must be the first entry, stored, no extra. */
    result = add_entry(zip, "mimetype", "application/epub+zip");
    if (result == ESP_OK) {
        result = add_entry(zip, "META-INF/container.xml",
                           "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                           "<container version=\"1.0\" "
                           "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:"
                           "container\">\n"
                           "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" "
                           "media-type=\"application/oebps-package+xml\"/>"
                           "</rootfiles>\n</container>\n");
    }
    if (result == ESP_OK) {
        result = add_entry(zip, "OEBPS/content.opf", opf);
    }
    if (result == ESP_OK) {
        result = add_entry(zip, "OEBPS/nav.xhtml", nav);
    }
    if (result == ESP_OK) {
        result = add_entry(zip, "OEBPS/toc.ncx", ncx);
    }
    if (result == ESP_OK) {
        result = add_entry(zip, "OEBPS/chapter.xhtml", chapter);
    }
    if (result != ESP_OK) {
        zip_writer_abort(zip);
        zip = NULL;
        goto cleanup;
    }
    result = zip_writer_close(zip);

cleanup:
    free(body);
    free(chapter);
    free(opf);
    free(nav);
    free(ncx);
    return result;
}
