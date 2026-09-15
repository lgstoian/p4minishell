/**
 * @file editor.h
 * @brief DOS-style inline text editor for P4MiniShell.
 *
 * A modal, touch-first text editor invoked with `edit <path>`. It edits any
 * byte-preserving text file (batch, txt, sys, or any other extension) with
 * full inline cursor editing, text selection, insert/overwrite mode, and
 * syntax highlighting for batch files. The whole basic feature set works from
 * the on-screen touch keyboard; a USB keyboard/mouse expands it automatically.
 *
 * Architecture:
 *   - editor_doc_t owns the byte-preserving line store plus the cursor and
 *     selection state. All edits go through editor_doc_*() operations.
 *   - The LVGL surface (editor_view.c) renders the document one row at a time
 *     with syntax-coloured spans, a block cursor, and a selection overlay.
 *     It is modal: while active it owns the transcript region and the input
 *     row becomes a status bar.
 *   - The `edit` command runs on the command worker task; it loads the file,
 *     opens the editor view via lv_async_call, then blocks until the user
 *     quits (or a save is requested). File I/O stays on the worker task.
 *
 * Thread safety: the document is owned by the LVGL task while the view is
 * open; the worker only touches it around open/close handoffs. All
 * editor_doc_* mutators must run on the LVGL task.
 */

#ifndef P4MINISHELL_EDITOR_H
#define P4MINISHELL_EDITOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "p4minishell_config.h"
#include "ansi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * MEMORY (PSRAM-FIRST)
 * ======================================================================== */

/**
 * Editor allocation helpers. Every large editor buffer - line text, the line
 * array, load staging, undo snapshots, and the view's per-row caches - goes
 * through these so a large file uses the PSRAM heap instead of the small,
 * fragmented internal DMA-capable heap that LVGL spans and SD DMA share.
 * Each falls back to the internal heap when PSRAM is unavailable.
 */
void *editor_mem_alloc(size_t size);
void *editor_mem_realloc(void *ptr, size_t size);
void editor_mem_free(void *ptr);

/* ========================================================================
 * DOCUMENT MODEL
 * ======================================================================== */

/** Syntax highlighting modes the editor can apply to a document. */
typedef enum {
    EDITOR_SYNTAX_PLAIN = 0,   /**< Single-colour text */
    EDITOR_SYNTAX_BATCH,       /**< DOS batch / .bat highlighting */
    EDITOR_SYNTAX_MARKDOWN,    /**< Markdown / .md highlighting */
    EDITOR_SYNTAX_JSON,        /**< JSON data highlighting */
    EDITOR_SYNTAX_COUNT
} editor_syntax_t;

/** Editing mode for a keystroke. */
typedef enum {
    EDITOR_KEY_NONE = 0,
    EDITOR_KEY_INSERT,        /**< Printable char at cursor */
    EDITOR_KEY_NEWLINE,       /**< Split the line (Enter) */
    EDITOR_KEY_BACKSPACE,     /**< Delete char before cursor */
    EDITOR_KEY_DELETE,        /**< Delete char at cursor (forward) */
    EDITOR_KEY_TAB,           /**< Insert spaces to the next tab stop */
    EDITOR_KEY_LEFT,          /**< Move cursor left */
    EDITOR_KEY_RIGHT,         /**< Move cursor right */
    EDITOR_KEY_UP,            /**< Move cursor up */
    EDITOR_KEY_DOWN,          /**< Move cursor down */
    EDITOR_KEY_HOME,          /**< Start of line (or file with SHIFT) */
    EDITOR_KEY_END,           /**< End of line (or file with SHIFT) */
    EDITOR_KEY_PAGE_UP,       /**< One viewport up */
    EDITOR_KEY_PAGE_DOWN,     /**< One viewport down */
    EDITOR_KEY_SELECT_ALL,    /**< Select the whole document */
    EDITOR_KEY_COPY,          /**< Copy selection to the RAM clipboard */
    EDITOR_KEY_CUT,           /**< Cut selection to the RAM clipboard */
    EDITOR_KEY_PASTE,         /**< Paste the RAM clipboard at the cursor */
    EDITOR_KEY_UNDO,          /**< Undo the last edit */
    EDITOR_KEY_REDO,          /**< Redo the last undone edit */
    EDITOR_KEY_SAVE,          /**< Save the document */
    EDITOR_KEY_SAVE_AS,       /**< Save the document to a new path */
    EDITOR_KEY_QUIT,          /**< Quit the editor (discard / confirm) */
    EDITOR_KEY_WORD_LEFT,     /**< Move cursor one word left */
    EDITOR_KEY_WORD_RIGHT,    /**< Move cursor one word right */
    EDITOR_KEY_DOC_HOME,      /**< Jump to the start of the document */
    EDITOR_KEY_DOC_END,       /**< Jump to the end of the document */
    EDITOR_KEY_DELETE_LINE,   /**< Delete the whole current line */
    EDITOR_KEY_DELETE_EOL,    /**< Delete from the cursor to end of line */
    EDITOR_KEY_OVERWRITE,     /**< Toggle insert / overwrite mode */
    EDITOR_KEY_FIND,          /**< Find (search forward) */
    EDITOR_KEY_FIND_NEXT,     /**< Repeat the last find */
    EDITOR_KEY_REPLACE,       /**< Replace (search + replace) */
    EDITOR_KEY_REPLACE_ALL,   /**< Replace every match (USB Ctrl+R) */
    EDITOR_KEY_CASE_TOGGLE,   /**< Toggle find case sensitivity (USB Ctrl+T) */
    EDITOR_KEY_COMMENT,       /**< Toggle line comments (USB Ctrl+/) */
    EDITOR_KEY_MATCH_JUMP,    /**< Jump to matching paren/% (USB Ctrl+B) */
    EDITOR_KEY_WRAP_TOGGLE,   /**< Toggle word wrap (USB Ctrl+W) */
    EDITOR_KEY_RELOAD,        /**< Reload file from disk (USB Ctrl+L) */
    EDITOR_KEY_OPEN,          /**< Open another file (touch `Open` / USB F4) */
    EDITOR_KEY_GOTO_LINE,     /**< Jump to a line number */
    EDITOR_KEY_PREVIEW,       /**< Toggle rendered Markdown preview (USB Ctrl+P) */
} editor_key_t;

/** One line of the document: a byte buffer that never contains '\n' or '\r'. */
typedef struct {
    char *text;               /**< Heap line content (may hold 8-bit bytes) */
    size_t length;            /**< Number of bytes in text (no NUL guarantee) */
    size_t capacity;          /**< Allocated capacity of text */
} editor_line_t;

/**
 * The byte-preserving editor document.
 *
 * Lines keep every byte exactly as loaded (tabs and 8-bit characters
 * included). A logical line is a single editor_line_t; the EOL style of the
 * file is remembered so a save round-trips unchanged. The cursor is a (row,
 * column) pair where column is a byte offset into the line.
 */
typedef struct {
    editor_line_t *lines;     /**< Array of lines */
    size_t line_count;        /**< Number of lines */
    size_t line_capacity;     /**< Allocated line slots */

    bool crlf;                /**< true: CRLF endings; false: bare LF */
    bool trailing_newline;    /**< File ended with a newline on load */

    size_t cursor_row;        /**< Active line (0-based) */
    size_t cursor_col;        /**< Byte column within the active line */

    bool selection_active;    /**< A selection exists */
    size_t sel_row;           /**< Selection anchor row */
    size_t sel_col;           /**< Selection anchor column */

    bool overwrite;           /**< Insert mode (false) or overwrite (true) */
    bool modified;            /**< Unsaved changes */

    /**
     * Approximate serialized size of the document (the last snapshot length or
     * the loaded file size). Used to decide whether a full-document undo
     * snapshot per edit is affordable; it is not a live byte count.
     */
    size_t content_bytes;

    bool readonly;            /**< Source file is read-only (save refused) */

    char path[P4_CONFIG_SD_PATH_BYTES]; /**< Source path, "" when unnamed */

    editor_syntax_t syntax;   /**< Syntax highlighting mode */

    /**
     * Opaque undo/redo ring storage. Sized for two rings of
     * P4_CONFIG_EDITOR_UNDO_DEPTH snapshot descriptors (each ~72 bytes of
     * metadata; the document bytes themselves live on the heap). Owned by
     * editor.c; never touched outside this module.
     */
    unsigned char undo_storage[P4_CONFIG_EDITOR_UNDO_DEPTH * 2 * 80];
} editor_doc_t;

/* ========================================================================
 * DOCUMENT LIFECYCLE
 * ======================================================================== */

/** Create an empty document bound to @p path (may be "" for unnamed). */
editor_doc_t *editor_doc_new(const char *path);

/** Load a file into a new document. Path must resolve on the SD card.
 *  Returns NULL on failure (missing file, too large, allocation failure). */
editor_doc_t *editor_doc_load(const char *path);

/** Free a document and all its lines. */
void editor_doc_free(editor_doc_t *doc);

/** Set the syntax mode from the document path (batch vs plain). */
void editor_doc_pick_syntax(editor_doc_t *doc);

/* ========================================================================
 * DOCUMENT ACCESS
 * ======================================================================== */

/** Return the number of lines in the document. */
size_t editor_doc_line_count(const editor_doc_t *doc);

/** Return the length of the row-th line in bytes. */
size_t editor_doc_line_length(const editor_doc_t *doc, size_t row);

/** Return the row-th line's bytes (may not be NUL-terminated). */
const char *editor_doc_line_text(const editor_doc_t *doc, size_t row);

/** Cursor row. */
size_t editor_doc_cursor_row(const editor_doc_t *doc);

/** Cursor column. */
size_t editor_doc_cursor_col(const editor_doc_t *doc);

/** Whether the document has unsaved changes. */
bool editor_doc_is_modified(const editor_doc_t *doc);

/** Whether the document is byte-identical to the file it was loaded from. */
bool editor_doc_needs_save(const editor_doc_t *doc);

/* ========================================================================
 * EDITING OPERATIONS (all run on the LVGL task)
 * ======================================================================== */

/** Insert a single character at the cursor (respects overwrite mode). */
void editor_doc_insert_char(editor_doc_t *doc, char ch);

/** Insert a run of bytes at the cursor. */
void editor_doc_insert_bytes(editor_doc_t *doc, const char *bytes, size_t len);

/** Split the line at the cursor (Enter). */
void editor_doc_newline(editor_doc_t *doc);

/** Delete the character before the cursor. */
void editor_doc_backspace(editor_doc_t *doc);

/** Delete the character at the cursor (forward). */
void editor_doc_delete(editor_doc_t *doc);

/** Insert spaces up to the next tab stop. */
void editor_doc_tab(editor_doc_t *doc);

/** Move the cursor one position in the given direction. */
void editor_doc_cursor_left(editor_doc_t *doc);
void editor_doc_cursor_right(editor_doc_t *doc);
void editor_doc_cursor_up(editor_doc_t *doc);
void editor_doc_cursor_down(editor_doc_t *doc);

/** Move the cursor to the start / end of the line. */
void editor_doc_cursor_home(editor_doc_t *doc);
void editor_doc_cursor_end(editor_doc_t *doc);

/** Move the cursor one word left / right (whitespace-delimited). */
void editor_doc_cursor_word_left(editor_doc_t *doc);
void editor_doc_cursor_word_right(editor_doc_t *doc);

/** Jump to the start / end of the whole document. */
void editor_doc_cursor_doc_home(editor_doc_t *doc);
void editor_doc_cursor_doc_end(editor_doc_t *doc);

/** Toggle insert / overwrite mode. */
void editor_doc_toggle_overwrite(editor_doc_t *doc);

/** Delete the whole current line (joining the neighbours). */
void editor_doc_delete_line(editor_doc_t *doc);

/** Delete from the cursor to the end of the current line. */
void editor_doc_delete_to_eol(editor_doc_t *doc);

/**
 * Search forward for @p needle starting at (start_row, start_col) (inclusive).
 * When @p wrap is set the search continues past the end of the document back
 * to the start. Pure function: never mutates the document.
 * @return true and the (row, col) of the match when found.
 */
bool editor_doc_find_next(const editor_doc_t *doc,
                          const char *needle, size_t needle_len,
                          size_t start_row, size_t start_col,
                          bool case_sensitive, bool wrap,
                          size_t *out_row, size_t *out_col);

/**
 * Find @p needle from the cursor and replace it with @p replacement (one
 * match per call). A no-op when the needle is empty or not found. Leaves the
 * cursor just past the inserted replacement so a repeated call walks the
 * document.
 * @return true when a replacement was performed.
 */
bool editor_doc_replace_next(editor_doc_t *doc,
                             const char *needle, size_t needle_len,
                             const char *replacement, size_t repl_len,
                             bool case_sensitive,
                             size_t *out_row, size_t *out_col);

/**
 * Replace every match of @p needle with @p replacement (whole document,
 * non-overlapping, replacements never re-match). One undo snapshot covers
 * the whole operation; a fruitless call takes none. Cursor lands after the
 * last replacement.
 * @return the number of replacements performed.
 */
size_t editor_doc_replace_all(editor_doc_t *doc,
                              const char *needle, size_t needle_len,
                              const char *replacement, size_t repl_len,
                              bool case_sensitive);

/**
 * Toggle line comments over the selection (or cursor row) using the
 * document syntax: batch `rem ` (also strips `::`), json `// `,
 * markdown `<!-- ... -->`. Blank lines skipped. All-commented ranges
 * uncomment, otherwise everything comments. One undo snapshot.
 * Plain/unknown syntax is a no-op (returns 0).
 * @return lines changed.
 */
size_t editor_doc_comment_toggle(editor_doc_t *doc);

/**
 * Jump from a paren or % to its match (cursor on the bracket, or just
 * past it at EOL). Parens nest across the document, skipping quoted spans
 * and whole-line rem/:: comments; %var% pairs match within one line
 * (forward first, then backward). Clears the selection on success.
 * @return true when the cursor moved.
 */
bool editor_doc_match_jump(editor_doc_t *doc);

/** Change the document's save path (Save As). "" unbinds the path. */
void editor_doc_set_path(editor_doc_t *doc, const char *path);

/** Report whether @p path (already resolved) names a missing file. */
bool editor_file_missing(const char *resolved_path);

/* ========================================================================
 * SELECTION
 * ======================================================================== */

/** Start a selection at the current cursor (call before a shift-move). */
void editor_doc_selection_begin(editor_doc_t *doc);

/** Extend the selection to the current cursor (after a move). */
void editor_doc_selection_extend(editor_doc_t *doc);

/** Report whether a selection is currently active. */
bool editor_doc_has_selection(const editor_doc_t *doc);

/** Clear the active selection (without touching the cursor). */
void editor_doc_selection_clear(editor_doc_t *doc);

/** Normalize and return selection bounds (start <= end). */
void editor_doc_selection_bounds(const editor_doc_t *doc,
                                 size_t *s_row, size_t *s_col,
                                 size_t *e_row, size_t *e_col);

/** Copy the selection to the RAM clipboard (shell_clipboard). */
bool editor_doc_selection_copy(const editor_doc_t *doc);

/** Delete the active selection, leaving the cursor at its start. */
void editor_doc_selection_delete(editor_doc_t *doc);

/** Select the entire document. */
void editor_doc_select_all(editor_doc_t *doc);

/** Cut the selection into the RAM clipboard. */
bool editor_doc_selection_cut(editor_doc_t *doc);

/** Paste the RAM clipboard at the cursor. */
void editor_doc_paste(editor_doc_t *doc);

/* ========================================================================
 * UNDO / REDO
 * ======================================================================== */

/** Record an undo snapshot of the current document (call before an edit)
 *  and clear the redo history. */
void editor_doc_undo_mark(editor_doc_t *doc);

/** Undo the last editing operation. */
void editor_doc_undo(editor_doc_t *doc);

/** Redo the last undone operation. */
void editor_doc_redo(editor_doc_t *doc);

/* ========================================================================
 * SAVE
 * ======================================================================== */

/**
 * Serialize the document to the file it was opened from (or the given path).
 * A pre-existing destination is first copied to "<file>.bak" (best-effort).
 * Returns ESP_OK on success. Runs on the worker task.
 */
esp_err_t editor_doc_save(editor_doc_t *doc, const char *path);

/**
 * Re-read the document from its bound path, discarding unsaved changes
 * (cursor clamped, undo reset, modified cleared). Unnamed buffers and
 * missing/unreadable files fail. Runs on the worker task.
 */
esp_err_t editor_doc_reload(editor_doc_t *doc);

/* ========================================================================
 * KEY ROUTING
 * ======================================================================== */

/** Map an ASCII character + USB key code + modifiers to an editor action.
 *  Returns EDITOR_KEY_NONE when the key does not map to an editor action.
 *  @p ascii may be '\0' when only a key code is meaningful. */
editor_key_t editor_key_from_usb(uint8_t key_code, uint8_t modifiers, char ascii);

/* ========================================================================
 * BATCH LEXER
 * ======================================================================== */

/** One syntax-coloured run of a line. @p index is an ANSI color index. */
typedef struct {
    size_t start;              /**< Byte offset into the line */
    size_t length;             /**< Run length in bytes */
    ansi_color_index_t color;  /**< Palette colour for the run */
    unsigned attrs;            /**< ANSI_ATTR_* bitmask (bold/italic/...), 0 plain */
} editor_syntax_run_t;

/** Lex @p text into colour runs for batch syntax.
 *  @p runs and @p capacity describe the caller's run buffer.
 *  @return the number of runs produced (<= capacity). */
size_t editor_lex_batch(const char *text, size_t len,
                        editor_syntax_run_t *runs, size_t capacity);

/** Lex one Markdown source line into runs (headings bold, code yellow,
 * links cyan+underline, markers green). Fences are per-line only (no
 * multi-line state): a ``` line highlights whole as code. */
size_t editor_lex_markdown(const char *text, size_t len,
                           editor_syntax_run_t *runs, size_t capacity);

/** Lex one JSON source line into runs (keys cyan, strings green, numbers
 * yellow, true/false/null magenta, punctuation white). Unterminated strings
 * run to EOL so the error is visible. */
size_t editor_lex_json(const char *text, size_t len,
                       editor_syntax_run_t *runs, size_t capacity);

/* ========================================================================
 * LINE-NUMBER GUTTER (pure)
 * ======================================================================== */

/**
 * Right-align the 1-based line number into @p width digits followed by one
 * space (the visual gutter cell used by the editor's line-number column).
 * Pure and unit-testable; the LVGL surface feeds it the rendered gutter.
 *
 * @param line     0-based line index (the printed number is line + 1).
 * @param width    Gutter width in digits (P4_CONFIG_EDITOR_LINE_NUMBER_WIDTH_CHARS).
 * @param out      Destination buffer.
 * @param out_size Capacity of @p out.
 * @return The number of characters written (without the NUL terminator), or 0
 *         when the buffer is too small.
 */
size_t editor_format_line_number(size_t line, unsigned width,
                                 char *out, size_t out_size);

/* ========================================================================
 * EDITOR SESSION (worker task)
 * ======================================================================== */

typedef struct editor_session editor_session_t;

/**
 * Run a modal `edit <path>` session on the command worker task.
 *
 * Loads the file (or starts a new buffer when the path does not exist), opens
 * the LVGL editor view, and blocks until the user quits. Saves requested from
 * the view are performed here (file I/O stays off the LVGL task).
 *
 * @param path       Source path ("" or NULL opens an unnamed buffer).
 * @param errorlevel Receives the command errorlevel (0 on clean exit).
 * @return ESP_OK on success.
 */
esp_err_t editor_session_run(const char *path, int *errorlevel);

/** Report whether a modal editor session is running (view open or opening).
 *  Used by the UART console reader to route serial input to the editor even
 *  during the async view-open window. */
bool editor_session_is_active(void);

/* ========================================================================
 * SESSION CONTROL (shared between worker and LVGL tasks)
 * ======================================================================== */

/**
 * Control block handed to the editor view. The worker task owns it for the
 * session; the view mutates only the flags and signals @p event_group. The
 * document pointer is owned by the worker and only touched by the view.
 *
 * Session event bits for the shared modal event group. MODAL_EVENT_* live in
 * modal.h; the editor SAVE/RELOAD/OPEN bits live here so the worker and the
 * LVGL view cannot disagree (one definition).
 */
#define EDITOR_EVENT_SAVE   (1u << 2)
#define EDITOR_EVENT_RELOAD (1u << 3)
#define EDITOR_EVENT_OPEN   (1u << 4)

typedef struct {
    editor_doc_t *doc;                /**< Document being edited */
    void *event_group;                /**< EventGroupHandle_t (worker -> view) */
    bool save_requested;              /**< Set by view: worker should save */
    bool quit_requested;              /**< Set by view: worker should close */
    bool save_ok;                     /**< Set by worker: last save succeeded */
    bool reload_requested;            /**< Set by view: worker should reload */
    bool reload_ok;                   /**< Set by worker: last reload succeeded */
    bool open_requested;              /**< Set by view: worker should open another file */
    bool open_ok;                     /**< Set by worker: last open succeeded */
    char save_as_path[P4_CONFIG_EDITOR_PROMPT_BYTES]; /**< Save As target; "" = source path */
    char open_path[P4_CONFIG_EDITOR_PROMPT_BYTES];    /**< Open target path */
} editor_control_t;

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_EDITOR_H */
