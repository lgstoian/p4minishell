/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file epub.c
 * @brief EPUB reader main implementation.
 *
 * Orchestrates ZIP extraction, OPF/NCX/nav parsing, and chapter rendering.
 */

#include "epub.h"
#include "epub_internal.h"
#include "html.h"
#include "zip.h"
#include "archive.h"
#include "storage.h"
#include "shell.h"
#include "p4minishell_config.h"
#include "strutil.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <p4heap.h>
#include <expat.h>


/* ------------------------------------------------------------------------- */
/* Internal structures                                                       */
/* ------------------------------------------------------------------------- */

/* struct epub_t, the manifest-item type, and the EPUB_MAX_* capacities live in
 * epub_internal.h so epub_render.c reads the same layout. */

#define EPUB_MAX_ENTRIES     512
#define EPUB_LOG_TAG "epub"
#define EPUB_TEMP_DIR        "/sd/tmp/epub_XXXXXX"

/* ------------------------------------------------------------------------- */
/* XML parsing callbacks (Expat)                                            */
/* ------------------------------------------------------------------------- */

typedef struct {
    epub_t *epub;
    int depth;
    char current_element[64];
    char current_attr_name[64];
    char current_attr_value[256];
    char text_buffer[1024];
    int text_len;

    /* OPF parsing state */
    bool in_metadata;
    bool in_manifest;
    bool in_spine;
    bool in_guide;
    epub_manifest_item_internal_t *current_manifest;
    epub_chapter_t *current_chapter;
    char current_spine_idref[64];

    /* NCX/nav parsing state */
    bool in_nav_map;
    bool in_nav_point;
    bool in_nav_label;
    bool in_nav_content;
    epub_toc_entry_t *current_toc;
    int toc_depth;
} epub_xml_parser_t;

/* Forward declarations */
static void epub_xml_start(void *user_data, const char *name, const char **atts);
static void epub_xml_end(void *user_data, const char *name);
static void epub_xml_text(void *user_data, const char *s, int len);

static void epub_xml_start(void *user_data, const char *name, const char **atts)
{
    epub_xml_parser_t *p = (epub_xml_parser_t *)user_data;
    p->depth++;
    strlcpy(p->current_element, name, sizeof(p->current_element));
    p->text_len = 0;

    if (strcmp(name, "metadata") == 0) {
        p->in_metadata = true;
    } else if (strcmp(name, "manifest") == 0) {
        p->in_manifest = true;
    } else if (strcmp(name, "spine") == 0) {
        p->in_spine = true;
    } else if (strcmp(name, "guide") == 0) {
        p->in_guide = true;
    } else if (strcmp(name, "item") == 0 && p->in_manifest) {
        if (p->epub->manifest_count < EPUB_MAX_MANIFEST) {
            p->current_manifest = &p->epub->manifest[p->epub->manifest_count++];
            memset(p->current_manifest, 0, sizeof(*p->current_manifest));
        }
    } else if (strcmp(name, "itemref") == 0 && p->in_spine) {
        if (p->epub->spine_count < EPUB_MAX_SPINE) {
            p->current_chapter = &p->epub->spine[p->epub->spine_count++];
            memset(p->current_chapter, 0, sizeof(*p->current_chapter));
        }
    } else if (strcmp(name, "navMap") == 0) {
        p->in_nav_map = true;
    } else if (strcmp(name, "navPoint") == 0) {
        p->in_nav_point = true;
        p->toc_depth++;
        if (p->epub->toc_count < EPUB_MAX_TOC) {
            p->current_toc = &p->epub->toc[p->epub->toc_count++];
            memset(p->current_toc, 0, sizeof(*p->current_toc));
            p->current_toc->depth = p->toc_depth - 1;
        }
    } else if (strcmp(name, "navLabel") == 0) {
        p->in_nav_label = true;
    } else if (strcmp(name, "content") == 0 && p->in_nav_point) {
        p->in_nav_content = true;
    } else if (strcmp(name, "nav") == 0) {
        /* EPUB3 nav.xhtml */
    }

    /* Process attributes */
    for (int i = 0; atts[i]; i += 2) {
        const char *attr_name = atts[i];
        const char *attr_value = atts[i + 1];

        strlcpy(p->current_attr_name, attr_name, sizeof(p->current_attr_name));
        strlcpy(p->current_attr_value, attr_value, sizeof(p->current_attr_value));

        if (p->in_metadata) {
            if (strcmp(attr_name, "id") == 0 && strstr(p->current_element, "dc:") == p->current_element) {
                /* dc:title, dc:creator, etc. handled in text */
            }
        } else if (p->in_manifest && p->current_manifest) {
            if (strcmp(attr_name, "id") == 0) {
                strlcpy(p->current_manifest->id, attr_value, sizeof(p->current_manifest->id));
            } else if (strcmp(attr_name, "href") == 0) {
                strlcpy(p->current_manifest->href, attr_value, sizeof(p->current_manifest->href));
            } else if (strcmp(attr_name, "media-type") == 0) {
                strlcpy(p->current_manifest->media_type, attr_value, sizeof(p->current_manifest->media_type));
            }
        } else if (p->in_spine && p->current_chapter) {
            if (strcmp(attr_name, "idref") == 0) {
                strlcpy(p->current_chapter->id, attr_value, sizeof(p->current_chapter->id));
            } else if (strcmp(attr_name, "linear") == 0) {
                p->current_chapter->linear = (strcmp(attr_value, "yes") == 0);
            }
        } else if (p->in_nav_point && p->in_nav_content) {
            if (strcmp(attr_name, "src") == 0) {
                strlcpy(p->current_toc->href, attr_value, sizeof(p->current_toc->href));
            }
        } else if (p->in_nav_content) {
            /* nav.xhtml <a href="..."> */
            if (strcmp(attr_name, "href") == 0 && p->current_toc) {
                strlcpy(p->current_toc->href, attr_value, sizeof(p->current_toc->href));
            }
        }
    }
}

static void epub_xml_end(void *user_data, const char *name)
{
    epub_xml_parser_t *p = (epub_xml_parser_t *)user_data;

    if (strcmp(name, "metadata") == 0) {
        p->in_metadata = false;
    } else if (strcmp(name, "manifest") == 0) {
        p->in_manifest = false;
    } else if (strcmp(name, "spine") == 0) {
        p->in_spine = false;
    } else if (strcmp(name, "guide") == 0) {
        p->in_guide = false;
    } else if (strcmp(name, "item") == 0) {
        p->current_manifest = NULL;
    } else if (strcmp(name, "itemref") == 0) {
        /* Resolve idref to manifest item */
        if (p->current_chapter && p->current_chapter->id[0]) {
            for (int i = 0; i < p->epub->manifest_count; i++) {
                if (strcmp(p->epub->manifest[i].id, p->current_chapter->id) == 0) {
                    strlcpy(p->current_chapter->href, p->epub->manifest[i].href, sizeof(p->current_chapter->href));
                    break;
                }
            }
        }
        p->current_chapter = NULL;
    } else if (strcmp(name, "navMap") == 0) {
        p->in_nav_map = false;
    } else if (strcmp(name, "navPoint") == 0) {
        p->in_nav_point = false;
        p->toc_depth--;
        p->current_toc = NULL;
    } else if (strcmp(name, "navLabel") == 0) {
        p->in_nav_label = false;
    } else if (strcmp(name, "content") == 0) {
        p->in_nav_content = false;
    }

    p->depth--;
}

static void epub_xml_text(void *user_data, const char *s, int len)
{
    epub_xml_parser_t *p = (epub_xml_parser_t *)user_data;

    if (p->in_metadata) {
        if (strcmp(p->current_element, "dc:title") == 0) {
            size_t n = (len < sizeof(p->epub->metadata.title) - 1 - p->text_len) ? len : sizeof(p->epub->metadata.title) - 1 - p->text_len;
            if (n > 0) {
                memcpy(p->epub->metadata.title + p->text_len, s, n);
                p->text_len += n;
                p->epub->metadata.title[p->text_len] = '\0';
            }
        } else if (strcmp(p->current_element, "dc:creator") == 0) {
            size_t n = (len < sizeof(p->epub->metadata.author) - 1 - p->text_len) ? len : sizeof(p->epub->metadata.author) - 1 - p->text_len;
            if (n > 0) {
                memcpy(p->epub->metadata.author + p->text_len, s, n);
                p->text_len += n;
                p->epub->metadata.author[p->text_len] = '\0';
            }
        } else if (strcmp(p->current_element, "dc:language") == 0) {
            size_t n = (len < sizeof(p->epub->metadata.language) - 1 - p->text_len) ? len : sizeof(p->epub->metadata.language) - 1 - p->text_len;
            if (n > 0) {
                memcpy(p->epub->metadata.language + p->text_len, s, n);
                p->text_len += n;
                p->epub->metadata.language[p->text_len] = '\0';
            }
        } else if (strcmp(p->current_element, "dc:identifier") == 0) {
            size_t n = (len < sizeof(p->epub->metadata.identifier) - 1 - p->text_len) ? len : sizeof(p->epub->metadata.identifier) - 1 - p->text_len;
            if (n > 0) {
                memcpy(p->epub->metadata.identifier + p->text_len, s, n);
                p->text_len += n;
                p->epub->metadata.identifier[p->text_len] = '\0';
            }
        } else if (strcmp(p->current_element, "dc:publisher") == 0) {
            size_t n = (len < sizeof(p->epub->metadata.publisher) - 1 - p->text_len) ? len : sizeof(p->epub->metadata.publisher) - 1 - p->text_len;
            if (n > 0) {
                memcpy(p->epub->metadata.publisher + p->text_len, s, n);
                p->text_len += n;
                p->epub->metadata.publisher[p->text_len] = '\0';
            }
        } else if (strcmp(p->current_element, "dc:date") == 0) {
            size_t n = (len < sizeof(p->epub->metadata.date) - 1 - p->text_len) ? len : sizeof(p->epub->metadata.date) - 1 - p->text_len;
            if (n > 0) {
                memcpy(p->epub->metadata.date + p->text_len, s, n);
                p->text_len += n;
                p->epub->metadata.date[p->text_len] = '\0';
            }
        } else if (strcmp(p->current_element, "dc:description") == 0) {
            size_t n = (len < sizeof(p->epub->metadata.description) - 1 - p->text_len) ? len : sizeof(p->epub->metadata.description) - 1 - p->text_len;
            if (n > 0) {
                memcpy(p->epub->metadata.description + p->text_len, s, n);
                p->text_len += n;
                p->epub->metadata.description[p->text_len] = '\0';
            }
        }
    } else if (p->in_nav_label) {
        if (p->current_toc) {
            size_t n = (len < sizeof(p->current_toc->label) - 1 - p->text_len) ? len : sizeof(p->current_toc->label) - 1 - p->text_len;
            if (n > 0) {
                memcpy(p->current_toc->label + p->text_len, s, n);
                p->text_len += n;
                p->current_toc->label[p->text_len] = '\0';
            }
        }
    } else if (p->in_nav_content && p->current_toc) {
        /* nav.xhtml <a> text content */
        size_t n = (len < sizeof(p->current_toc->label) - 1 - p->text_len) ? len : sizeof(p->current_toc->label) - 1 - p->text_len;
        if (n > 0) {
            memcpy(p->current_toc->label + p->text_len, s, n);
            p->text_len += n;
            p->current_toc->label[p->text_len] = '\0';
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Internal helpers                                                          */
/* ------------------------------------------------------------------------- */

static bool epub_find_container_xml(epub_t *epub)
{
    void *zip = epub->zip_handle;
    zip_entry_t entry;
    if (zip_find_entry(zip, "META-INF/container.xml", &entry) != ESP_OK) {
        return false;
    }

    /* Extract and parse container.xml */
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s/container.xml", epub->temp_dir);
    if (zip_extract_entry(zip, &entry, tmp_path) != ESP_OK) {
        return false;
    }

    FILE *f = fopen(tmp_path, "r");
    if (!f) return false;

    char buf[4096];
    size_t len = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[len] = '\0';

    /* Parse for rootfile full-path */
    char *rootfile = strstr(buf, "rootfile");
    if (!rootfile) return false;
    char *fullpath = strstr(rootfile, "full-path");
    if (!fullpath) return false;
    fullpath = strchr(fullpath, '"');
    if (!fullpath) return false;
    fullpath++;
    char *end = strchr(fullpath, '"');
    if (!end) return false;
    *end = '\0';

    strlcpy(epub->opf_path, fullpath, sizeof(epub->opf_path));

    /* Extract directory of OPF */
    char *slash = strrchr(epub->opf_path, '/');
    if (slash) {
        *slash = '\0';
        strlcpy(epub->opf_dir, epub->opf_path, sizeof(epub->opf_dir));
        *slash = '/';
    } else {
        epub->opf_dir[0] = '.';
        epub->opf_dir[1] = '\0';
    }

    return true;
}

static bool epub_parse_opf_file(epub_t *epub)
{

    /* Find OPF entry in ZIP */
    if (epub->opf_dir[0] == '.') {
        epub->opf_dir[0] = '\0';
    }

    /* basename() of the OPF path. strrchr can return NULL when the path has
     * no '/', so fall back to the whole string rather than dereferencing. */
    const char *opf_base = strrchr(epub->opf_path, '/');
    opf_base = (opf_base != NULL) ? opf_base + 1 : epub->opf_path;

    char opf_zip_path_full[EPUB_PATH_MAX];
    if (epub->opf_dir[0]) {
        int n = snprintf(opf_zip_path_full, sizeof(opf_zip_path_full), "%s/%s",
                         epub->opf_dir, opf_base);

        if (n < 0 || (size_t)n >= sizeof(opf_zip_path_full)) {
            ESP_LOGE(EPUB_LOG_TAG, "OPF path too long to resolve");
            return false;
        }
    } else {
        strlcpy(opf_zip_path_full, opf_base, sizeof(opf_zip_path_full));
    }

    zip_entry_t entry;
    if (zip_find_entry(epub->zip_handle, opf_zip_path_full, &entry) != ESP_OK) {
        /* Try just the filename */
        char *fname = strrchr(epub->opf_path, '/');
        if (fname) fname++; else fname = epub->opf_path;
        if (zip_find_entry(epub->zip_handle, fname, &entry) != ESP_OK) {
            ESP_LOGE("epub", "OPF not found in archive");
            return false;
        }
    }

    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s/package.opf", epub->temp_dir);
    if (zip_extract_entry(epub->zip_handle, &entry, tmp_path) != ESP_OK) {
        return false;
    }

    FILE *f = fopen(tmp_path, "r");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 200000) { fclose(f); return false; }

    char *xml = malloc(len + 1);
    if (!xml) { fclose(f); return false; }
    size_t read = fread(xml, 1, len, f);
    fclose(f);
    xml[read] = '\0';

    epub_xml_parser_t parser = {0};
    parser.epub = (epub_t *)epub;

    /* Named xml_parser, not parser: the epub parser state above already owns
     * that identifier, and the two are different types. */
    XML_Parser xml_parser = XML_ParserCreate("UTF-8");
    if (xml_parser == NULL) {
        ESP_LOGE(EPUB_LOG_TAG, "XML_ParserCreate failed");
        free(xml);
        return false;
    }
    XML_SetUserData(xml_parser, &parser);
    XML_SetElementHandler(xml_parser, epub_xml_start, epub_xml_end);
    XML_SetCharacterDataHandler(xml_parser, epub_xml_text);

    if (!XML_Parse(xml_parser, xml, read, 1)) {
        ESP_LOGE(EPUB_LOG_TAG, "OPF parse error: %s at line %lu",
                 XML_ErrorString(XML_GetErrorCode(xml_parser)),
                 XML_GetCurrentLineNumber(xml_parser));
        free(xml);
        XML_ParserFree(xml_parser);
        return false;
    }

    free(xml);
    XML_ParserFree(xml_parser);
    return true;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

esp_err_t epub_open(const char *archive_path, epub_handle_t **out_handle)
{
    if (!archive_path || !out_handle) return ESP_ERR_INVALID_ARG;

    /* Create temp directory for extraction */
    char temp_dir[64];
    snprintf(temp_dir, sizeof(temp_dir), EPUB_TEMP_DIR);
    if (!storage_mkdir_p(temp_dir)) {
        /* Try unique name */
        for (int i = 0; i < 100; i++) {
            snprintf(temp_dir, sizeof(temp_dir), "/sd/tmp/epub_%04d", i);
            if (storage_mkdir_p(temp_dir)) break;
        }
    }

    epub_t *epub = calloc(1, sizeof(epub_t));
    if (!epub) return ESP_ERR_NO_MEM;

    strlcpy(epub->archive_path, archive_path, sizeof(epub->archive_path));
    strlcpy(epub->temp_dir, temp_dir, sizeof(epub->temp_dir));
    epub->current_spine_index = -1;

    /* Open ZIP archive */
    esp_err_t err = zip_open(archive_path, &epub->zip_handle);
    if (err != ESP_OK) {
        free(epub);
        return err;
    }

    /* Find container.xml to locate OPF */
    if (!epub_find_container_xml(epub)) {
        ESP_LOGE("epub", "Failed to find container.xml");
        zip_close(epub->zip_handle);
        free(epub);
        return ESP_FAIL;
    }

    /* Parse OPF for metadata, manifest, spine */
    if (!epub_parse_opf_file(epub)) {
        zip_close(epub->zip_handle);
        free(epub);
        return ESP_FAIL;
    }

    /* TODO: Parse NCX/nav for TOC */

    *out_handle = epub;
    return ESP_OK;
}

void epub_close(epub_handle_t *handle)
{
    if (!handle) return;

    if (handle->zip_handle) {
        zip_close(handle->zip_handle);
    }

    /* Clean up temp directory (optional, could leave for caching) */
    /* storage_rmdir(handle->temp_dir); */

    free(handle);
}

esp_err_t epub_get_metadata(const epub_handle_t *handle, epub_metadata_t *out_meta)
{
    if (!handle || !out_meta) return ESP_ERR_INVALID_ARG;
    *out_meta = handle->metadata;
    return ESP_OK;
}

esp_err_t epub_get_spine(const epub_handle_t *handle,
                         epub_chapter_t *out_chapters,
                         int max_chapters,
                         int *out_count)
{
    if (!handle || !out_chapters || !out_count) return ESP_ERR_INVALID_ARG;
    int count = handle->spine_count < max_chapters ? handle->spine_count : max_chapters;
    memcpy(out_chapters, handle->spine, count * sizeof(epub_chapter_t));
    *out_count = count;
    return ESP_OK;
}

esp_err_t epub_get_toc(const epub_handle_t *handle,
                       epub_toc_entry_t *out_toc,
                       int max_entries,
                       int *out_count)
{
    if (!handle || !out_toc || !out_count) return ESP_ERR_INVALID_ARG;
    int count = handle->toc_count < max_entries ? handle->toc_count : max_entries;
    memcpy(out_toc, handle->toc, count * sizeof(epub_toc_entry_t));
    *out_count = count;
    return ESP_OK;
}

esp_err_t epub_get_nav(const epub_handle_t *handle, epub_nav_t *out_nav)
{
    if (!handle || !out_nav) return ESP_ERR_INVALID_ARG;
    out_nav->current = (handle->current_spine_index >= 0 && handle->current_spine_index < handle->spine_count)
                       ? &handle->spine[handle->current_spine_index] : NULL;
    out_nav->current_index = handle->current_spine_index;
    out_nav->total_chapters = handle->spine_count;
    return ESP_OK;
}

esp_err_t epub_set_chapter(epub_handle_t *handle, int index)
{
    if (!handle || index < 0 || index >= handle->spine_count) return ESP_ERR_INVALID_ARG;
    handle->current_spine_index = index;
    return ESP_OK;
}

int epub_get_spine_count(const epub_handle_t *handle)
{
    return (handle != NULL) ? handle->spine_count : 0;
}

esp_err_t epub_next_chapter(epub_handle_t *handle)
{
    if (!handle || handle->current_spine_index >= handle->spine_count - 1) return ESP_ERR_INVALID_STATE;
    handle->current_spine_index++;
    return ESP_OK;
}

esp_err_t epub_prev_chapter(epub_handle_t *handle)
{
    if (!handle || handle->current_spine_index <= 0) return ESP_ERR_INVALID_STATE;
    handle->current_spine_index--;
    return ESP_OK;
}

esp_err_t epub_goto_toc_entry(epub_handle_t *handle, int toc_index)
{
    if (!handle || toc_index < 0 || toc_index >= handle->toc_count) return ESP_ERR_INVALID_ARG;

    epub_toc_entry_t *toc = &handle->toc[toc_index];
    if (toc->chapter_index < 0) return ESP_ERR_INVALID_STATE;

    return epub_set_chapter(handle, toc->chapter_index);
}

esp_err_t epub_render_chapter(const epub_handle_t *handle,
                              char *out_buf,
                              size_t buf_size,
                              size_t *out_len)
{
    if (!handle || handle->current_spine_index < 0) return ESP_ERR_INVALID_STATE;

    const epub_chapter_t *ch = &handle->spine[handle->current_spine_index];
    if (!ch || !ch->href[0]) return ESP_ERR_INVALID_STATE;

    /* Build full path to chapter HTML */
    char chapter_path[EPUB_PATH_MAX];
    if (!epub_resolve_href(handle->opf_dir, ch->href, chapter_path, sizeof(chapter_path))) {
        return ESP_FAIL;
    }

    /* Extract chapter HTML to temp file */
    zip_entry_t entry;
    if (zip_find_entry(((epub_t *)handle)->zip_handle, chapter_path, &entry) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s/chapter.html", ((epub_t *)handle)->temp_dir);
    if (zip_extract_entry(((epub_t *)handle)->zip_handle, &entry, tmp_path) != ESP_OK) {
        return ESP_FAIL;
    }

    /* Read HTML file */
    FILE *f = fopen(tmp_path, "r");
    if (!f) return ESP_FAIL;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > P4_CONFIG_HTML_RENDER_MAX_BYTES) { fclose(f); return ESP_FAIL; }

    char *html = malloc(len + 1);
    if (!html) { fclose(f); return ESP_ERR_NO_MEM; }
    size_t read = fread(html, 1, len, f);
    fclose(f);
    html[read] = '\0';

    /* Render HTML to ANSI */
    size_t len_out = html_render_ansi(html, out_buf, buf_size);
    free(html);

    if (out_len) *out_len = len_out;
    return (len_out < buf_size || out_buf == NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t epub_extract_resource(const epub_handle_t *handle,
                                const char *href,
                                const char *dest_path)
{
    if (!handle || !href || !dest_path) return ESP_ERR_INVALID_ARG;

    char resource_path[EPUB_PATH_MAX];
    if (!epub_resolve_href(handle->opf_dir, href, resource_path, sizeof(resource_path))) {
        return ESP_ERR_NOT_FOUND;
    }

    void *zip = ((epub_t *)handle)->zip_handle;
    zip_entry_t entry;
    if (zip_find_entry(((epub_t *)handle)->zip_handle, resource_path, &entry) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    return zip_extract_entry(zip, &entry, dest_path);
}

bool epub_parse_opf(const char *xml, size_t len,
                    epub_metadata_t *out_meta,
                    epub_chapter_t *out_chapters,
                    int max_chapters,
                    int *out_count)
{
    /* This would need a full parser instance - use the internal parser above */
    return false; /* Not implemented as standalone - use epub_open */
}

bool epub_parse_ncx(const char *xml, size_t len,
                    epub_toc_entry_t *out_toc,
                    int max_entries,
                    int *out_count)
{
    /* NCX parsing - similar to OPF but for navMap/navPoint */
    return false; /* TODO */
}

bool epub_parse_nav(const char *xml, size_t len,
                    epub_toc_entry_t *out_toc,
                    int max_entries,
                    int *out_count)
{
    /* nav.xhtml parsing */
    return false; /* TODO */
}

bool epub_parse_manifest(const char *xml, size_t len,
                         epub_manifest_item_t *out_items,
                         int max_items,
                         int *out_count)
{
    /* Extract manifest from OPF - use internal parser */
    return false; /* Not standalone */
}

const epub_manifest_item_t *epub_find_manifest_by_id(const epub_manifest_item_t *items,
                                                      int count,
                                                      const char *id)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(items[i].id, id) == 0) return &items[i];
    }
    return NULL;
}

bool epub_resolve_href(const char *opf_dir, const char *href,
                       char *out_path, size_t out_size)
{
    if (!opf_dir || !href || !out_path) return false;

    if (href[0] == '/') {
        /* Absolute within archive - not typical for EPUB */
        return false;
    }

    char base[EPUB_PATH_MAX];
    if (opf_dir && opf_dir[0]) {
        snprintf(base, sizeof(base), "%s/", opf_dir);
    } else {
        base[0] = '\0';
    }

    /* Handle relative paths with ../ etc. */
    char resolved[EPUB_PATH_MAX];
    const char *src = href;
    char *dst = resolved;

    while (*src) {
        if (src[0] == '.' && src[1] == '.' && (src[2] == '/' || src[2] == '\0')) {
            /* Go up one directory */
            char *last_slash = strrchr(resolved, '/');
            if (last_slash) *last_slash = '\0';
            src += (src[2] == '/') ? 3 : 2;
        } else if (src[0] == '.' && (src[1] == '/' || src[1] == '\0')) {
            src += (src[1] == '/') ? 2 : 1;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';

    snprintf(out_path, out_size, "%s%s", base, resolved);
    return true;
}
