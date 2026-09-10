/**
 * @file filetype.c
 * @brief Central file-type registry (single extension table).
 */

#include "filetype.h"

#include <string.h>
#include <strings.h>

typedef struct {
    const char *ext; /* lowercase, with dot */
    filetype_t type;
} filetype_entry_t;

static const filetype_entry_t s_filetype_table[] = {
    { ".bat", FILETYPE_BATCH },
    { ".cmd", FILETYPE_BATCH },
    { ".md", FILETYPE_MARKDOWN },
    { ".markdown", FILETYPE_MARKDOWN },
    { ".mkd", FILETYPE_MARKDOWN },
    { ".json", FILETYPE_JSON },
    { ".txt", FILETYPE_TEXT },
    { ".log", FILETYPE_TEXT },
    { ".sys", FILETYPE_TEXT },
    { ".ini", FILETYPE_TEXT },
};

#define FILETYPE_TABLE_COUNT ((int)(sizeof(s_filetype_table) / sizeof(s_filetype_table[0])))

filetype_t filetype_of(const char *path)
{
    const char *dot;
    int i;

    if (path == NULL || path[0] == '\0') {
        return FILETYPE_UNKNOWN;
    }
    /* Extension = after the last dot; a leading dot (".profile") or a
     * trailing separator has no extension. */
    dot = strrchr(path, '.');
    if (dot == NULL || dot == path) {
        return FILETYPE_UNKNOWN;
    }
    {
        const char *sep = strrchr(path, '/');
        const char *sep2 = strrchr(path, '\\');
        const char *base = path;
        if (sep != NULL && sep > base) {
            base = sep + 1;
        }
        if (sep2 != NULL && sep2 > base) {
            base = sep2 + 1;
        }
        if (dot < base) {
            return FILETYPE_UNKNOWN;
        }
    }
    for (i = 0; i < FILETYPE_TABLE_COUNT; i++) {
        if (strcasecmp(dot, s_filetype_table[i].ext) == 0) {
            return s_filetype_table[i].type;
        }
    }
    return FILETYPE_UNKNOWN;
}

const char *filetype_name(filetype_t type)
{
    switch (type) {
    case FILETYPE_BATCH: return "batch";
    case FILETYPE_MARKDOWN: return "markdown";
    case FILETYPE_JSON: return "json";
    case FILETYPE_TEXT: return "text";
    default: return "unknown";
    }
}

bool filetype_is_executable(filetype_t type)
{
    return type == FILETYPE_BATCH;
}

bool filetype_is_markdown(filetype_t type)
{
    return type == FILETYPE_MARKDOWN;
}

bool filetype_has_extension(const char *path, const char *ext)
{
    size_t path_len;
    size_t ext_len;

    if (path == NULL || ext == NULL) {
        return false;
    }
    path_len = strlen(path);
    ext_len = strlen(ext);
    if (path_len < ext_len) {
        return false;
    }
    return strcasecmp(path + path_len - ext_len, ext) == 0;
}
