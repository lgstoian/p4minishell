/**
 * @file font.h
 * @brief Unified font registry with roles and fallback chains.
 *
 * Two roles: TERMINAL (pure monospace bitmap; transcript, TUI, editor,
 * viewers — anywhere cell metrics must be exact) and UI (chained with a
 * Montserrat fallback for FontAwesome PUA icons; header, input, keyboard,
 * buttons, dialogs). Only built-in bitmap fonts are selectable in Phase 1;
 * SD TTFs (any size) arrive in Phase 2. Sizes are fixed until then.
 */

#ifndef P4MINISHELL_FONT_H
#define P4MINISHELL_FONT_H

#include "lvgl.h"
#include "p4minishell_config.h"
#include "ansi.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Font roles. */
typedef enum {
    FONT_ROLE_TERMINAL = 0, /**< Monospace surfaces (transcript, TUI, editor). */
    FONT_ROLE_UI = 1,       /**< Chrome (header, input, keyboard, dialogs). */
    FONT_ROLE_COUNT
} font_role_t;

/** Display name of a role ("terminal"/"ui"). Never NULL. */
const char *font_role_name(font_role_t role);

/** Init defaults from P4_CONFIG_FONT_*_DEFAULT. Call once before windows_init. */
void font_init(void);
/** Font for a role. Never NULL (falls back to unscii_16). */
const lv_font_t *font_get(font_role_t role);

/** Current primary name for a role ("unscii_16"/...). Never NULL. */
const char *font_current_name(font_role_t role);

/** Built-in selectable fonts (Phase 1: bitmaps only). */
int font_builtin_count(void);
const char *font_builtin_name(int index);
bool font_builtin_monospace(int index);

/** Select a primary by name. Terminal refuses proportional fonts
 * (monospace rule for cell math). Returns false on unknown name/refusal. */
bool font_set(font_role_t role, const char *name);

/** Best-effort boot restore (NULL = keep current). False if any name invalid. */
bool font_restore(const char *terminal, const char *ui);

/* ========================================================================
 * SD TTF FONTS (Phase 3)
 * ========================================================================
 * Optional TrueType/OpenType fonts from sd:/FONTS/, rasterized on demand by
 * tiny_ttf (no freetype dep). Loaded lazily on first `font set`; built-ins
 * always work with or without SD. Sizes 10..28 px (P4_CONFIG_FONT_SIZE_MIN/
 * MAX); bitmap roles report their nominal 16 px and refuse sizing.
 */

/** Load (or reuse) sd:/FONTS/<stem>.ttf|.otf into a registry slot.
 * Detects monospace by measuring glyph advances. False on missing/corrupt
 * file or a full slot table — current fonts are untouched. */
bool font_load_ttf(const char *stem);

/** Stems of loadable .ttf/.otf files found in sd:/FONTS/ (0 without SD).
 * @p out holds up to @p cap NUL-terminated stems; returns the total found. */
int font_scan_ttf(char out[][P4_CONFIG_FONT_NAME_BYTES], int cap);

/** Nominal pixel size of a role (bitmap roles: 16). */
int font_current_size(font_role_t role);

/** Resize a TTF-backed role (bitmap roles refuse). Thread-safe: takes the
 * LVGL port lock around cache rebuilds. Restores from SHELL.INI at boot. */
bool font_set_size(font_role_t role, int px);

/** True when @p name is a known monospace font (builtin flag or measured
 * TTF probe). False for unknown names (indistinguishable from refusal). */
bool font_is_monospace(const char *name);

/** Max px at which @p name still fits 80x25 in a rect (terminal clamp).
 * Callers pass the keyboard-hidden height (width is keyboard-invariant).
 * -1 when the font cannot be loaded/measured. */
int font_terminal_max_px(const char *name, int rect_w, int rect_h);

/* ========================================================================
 * CJK FALLBACK (Phase 3)
 * ========================================================================
 * NotoSansSC auto-attaches as the tail of both roles' fallback chains
 * (primary -> montserrat-copy -> SC) whenever sd:/FONTS/NotoSansSC.* exists,
 * so CJK renders inline without ever becoming a selectable terminal primary
 * (monospace rule stands). No SD file: chains end at Montserrat (default
 * behavior, byte-identical). Reboot after removing font files.
 */

/** Best-effort attach of the CJK fallback for both roles (sizes track the
 * roles). Safe to call repeatedly; silent when the file is absent. */
void font_attach_cjk(void);

/* ========================================================================
 * VARIANTS + SPAN STYLING (markdown bold/italic)
 * ========================================================================
 * Bold/italic need real font variants (LVGL has no faux styles). Only the
 * DejaVuSansMono pair is vendored; everything else falls back to bright.
 */

#define FONT_VARIANT_BOLD   1
#define FONT_VARIANT_ITALIC 2

/** Variant object for (base stem, px, attr), or NULL when none exists.
/// The slot takes a permanent ref (never freed); bounded by the slot table. */
lv_font_t *font_variant_for(const char *base_stem, int px, int attr);

/** Apply attrs + color to a span style for a role: variant font when
 * available (bold preferred over italic), bright fallback for bold on
 * stock colors, LVGL decor for underline/strike. Centralizes transcript,
 * editor, and preview span styling. */
void font_span_style(lv_style_t *style, font_role_t role, unsigned attrs,
                     int fg_index, uint32_t fallback_rgb);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_FONT_H */
