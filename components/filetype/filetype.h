/**
 * @file filetype.h
 * @brief Central file-type registry for P4MiniShell.
 *
 * One table maps extensions to kinds; every component (editor syntax,
 * viewer rendering, launch discovery, dir colours, batch resolution)
 * asks here instead of hand-rolling strcasecmp chains. Leaf component:
 * pure string matching, no dependencies, no cycles.
 */

#ifndef P4MINISHELL_FILETYPE_H
#define P4MINISHELL_FILETYPE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** File kinds the firmware understands. */
typedef enum {
    FILETYPE_UNKNOWN = 0, /**< No special handling (opaque/view-as-text) */
    FILETYPE_BATCH,       /**< .bat/.cmd — executable, batch highlight */
    FILETYPE_MARKDOWN,    /**< .md/.markdown/.mkd — rendered view + highlight */
    FILETYPE_JSON,        /**< .json — highlight + pretty/validate */
    FILETYPE_TEXT,        /**< .txt/.log/.sys/.ini — plain viewer */
    FILETYPE_IMAGE,       /**< .bmp/.dib — BMP image viewer / canvas / TUI */
    FILETYPE_COUNT
} filetype_t;

/** Classify a path by extension (case-insensitive). Never fails;
 * returns FILETYPE_UNKNOWN for NULL, missing, or unknown extensions. */
filetype_t filetype_of(const char *path);

/** Stable kind name ("batch"/"markdown"/"json"/"text"/"unknown"). */
const char *filetype_name(filetype_t type);

/** True for executable script types (.bat/.cmd). */
bool filetype_is_executable(filetype_t type);

/** True for Markdown types (rendered view + preview). */
bool filetype_is_markdown(filetype_t type);

/** True for BMP image types (viewer / gfx canvas / TUI). */
bool filetype_is_image(filetype_t type);

/** True when @p path ends in @p ext (case-insensitive, ext with dot).
 * Used for one-off checks outside the registry (e.g. "%~x" parity). */
bool filetype_has_extension(const char *path, const char *ext);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_FILETYPE_H */
