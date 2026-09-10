/**
 * @file font.c
 * @brief Unified font registry with roles and fallback chains.
 *
 * Phase 1: built-in bitmap fonts only (unscii_16 monospace, montserrat_14
 * proportional + FontAwesome PUA subset). The UI role is a RAM copy of its
 * primary with .fallback chained (flash fonts are const; see the Phase-0
 * lesson in the changelog: never delegate via resolving callbacks —
 * lv_font_get_glyph_dsc() records the wrapper as resolved_font and bitmap
 * lookup recurses into itself).
 *
 * Phase 3: optional SD TTFs (tiny_ttf, lazy load from sd:/FONTS/) + pixel
 * sizes. Built-ins are immutable and always available; TTF failures never
 * disturb the current fonts.
 */

#include "font.h"
#include "font_fs.h"
#include "p4minishell_config.h"

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <dirent.h>
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "libs/tiny_ttf/lv_tiny_ttf.h"
#include "bsp/esp-bsp.h"

#define FONT_TAG "font"

/* Built-in bitmaps (always compiled: unscii via sdkconfig.defaults,
 * montserrat_14 as LVGL's default default-font). */
extern const lv_font_t lv_font_unscii_16;
extern const lv_font_t lv_font_montserrat_14;

typedef struct {
    const char *name;
    const lv_font_t *font;
    bool monospace;
} font_builtin_t;

static const font_builtin_t s_font_builtins[] = {
    { "unscii_16", &lv_font_unscii_16, true },
    { "montserrat_14", &lv_font_montserrat_14, false },
};

#define FONT_BUILTIN_COUNT ((int)(sizeof(s_font_builtins) / sizeof(s_font_builtins[0])))

/* TTF slot table (declared early: font_current_name reverse-resolves TTF
 * roles). Full lifecycle helpers live in the SD TTF section below. */
#define FONT_TTF_SLOTS P4_CONFIG_FONT_TTF_SLOTS

typedef struct {
    bool used;
    char stem[P4_CONFIG_FONT_NAME_BYTES];
    lv_font_t *font;
    int px;
    bool monospace;
    int refs; /* roles currently pointing at this object */
} font_ttf_slot_t;

static font_ttf_slot_t s_ttf_slots[FONT_TTF_SLOTS];
static SemaphoreHandle_t s_ttf_lock = NULL;

/* Per-role primary (source of truth for names/measuring) plus the render
 * chains: primary-copy -> FA-copy (Montserrat icons) -> NotoSansSC (CJK) or
 * NULL. Primary copies are double-buffered (publish by index flip) so an
 * LVGL render pass never sees a torn struct; FA copies only ever mutate
 * their .fallback pointer (atomic store), so they are single-buffered. */
static const lv_font_t *s_font_role_primary[FONT_ROLE_COUNT];
static lv_font_t s_chain[FONT_ROLE_COUNT][2];
static int s_chain_idx[FONT_ROLE_COUNT];
static bool s_chain_ready[FONT_ROLE_COUNT];
static lv_font_t s_fa_chain[FONT_ROLE_COUNT];
static bool s_fa_ready[FONT_ROLE_COUNT];
static int s_cjk_slot[FONT_ROLE_COUNT];
/* Per-role nominal pixel size (bitmaps stay 16) and TTF slot (-1 = builtin). */
static int s_role_px[FONT_ROLE_COUNT];
static int s_role_ttf[FONT_ROLE_COUNT];

static const font_builtin_t *font_find_builtin(const char *name)
{
    int i;

    if (name == NULL) {
        return NULL;
    }
    for (i = 0; i < FONT_BUILTIN_COUNT; i++) {
        if (strcasecmp(name, s_font_builtins[i].name) == 0) {
            return &s_font_builtins[i];
        }
    }
    return NULL;
}

/* Forward declarations (TTF lifecycle lives below; used by font_set). */
static void font_ttf_lock_init(void);
static void font_ttf_release_locked(int slot);
static int font_ttf_load_locked(const char *stem, int px);
static void font_refresh_cjk_locked(font_role_t role);

/* Ensure the FA copy for a role exists (metrics constant; only .fallback
 * mutates later, atomically). Caller holds s_ttf_lock. */
static void font_ensure_fa_locked(font_role_t role)
{
    if (s_fa_ready[role]) {
        return;
    }
    s_fa_chain[role] = lv_font_montserrat_14;
    s_fa_chain[role].fallback = NULL;
    s_fa_ready[role] = true;
}

/* Rebuild a role's primary chain copy (caller holds s_ttf_lock). */
static void font_rebuild_chain_locked(font_role_t role)
{
    int inactive;

    font_ensure_fa_locked(role);
    inactive = 1 - s_chain_idx[role];
    /* Plain struct copy under the port lock (LVGL heap object). On the
     * vanishingly unlikely failure the previous copy stays published. */
    if (lvgl_port_lock(0)) {
        s_chain[role][inactive] = *s_font_role_primary[role];
        s_chain[role][inactive].fallback = &s_fa_chain[role];
        s_chain_idx[role] = inactive;
        s_chain_ready[role] = true;
        lvgl_port_unlock();
    }
}

const char *font_role_name(font_role_t role)
{
    return role == FONT_ROLE_UI ? "ui" : "terminal";
}

void font_init(void)
{
    const font_builtin_t *term = font_find_builtin(P4_CONFIG_FONT_TERMINAL_DEFAULT);
    const font_builtin_t *ui = font_find_builtin(P4_CONFIG_FONT_UI_DEFAULT);

    /* Drive letter for sd:/FONTS (owned tiny driver; works with or without
     * a mounted card — opens simply fail until it mounts). */
    font_fs_init();

    /* Defensive: a misconfigured default can never leave a role NULL. */
    if (term == NULL || !term->monospace) {
        term = &s_font_builtins[0];
    }
    if (ui == NULL) {
        ui = &s_font_builtins[0];
    }
    s_font_role_primary[FONT_ROLE_TERMINAL] = term->font;
    s_font_role_primary[FONT_ROLE_UI] = ui->font;
    s_role_px[FONT_ROLE_TERMINAL] = P4_CONFIG_FONT_DEFAULT_PX;
    s_role_px[FONT_ROLE_UI] = P4_CONFIG_FONT_DEFAULT_PX;
    s_role_ttf[FONT_ROLE_TERMINAL] = -1;
    s_role_ttf[FONT_ROLE_UI] = -1;
    s_cjk_slot[FONT_ROLE_TERMINAL] = -1;
    s_cjk_slot[FONT_ROLE_UI] = -1;
    /* Chains (re)build lazily on first font_get. */
    s_chain_ready[FONT_ROLE_TERMINAL] = false;
    s_chain_ready[FONT_ROLE_UI] = false;
    s_fa_ready[FONT_ROLE_TERMINAL] = false;
    s_fa_ready[FONT_ROLE_UI] = false;
}

const lv_font_t *font_get(font_role_t role)
{
    if (role != FONT_ROLE_TERMINAL && role != FONT_ROLE_UI) {
        role = FONT_ROLE_TERMINAL;
    }
    if (!s_chain_ready[role]) {
        font_ttf_lock_init();
        xSemaphoreTake(s_ttf_lock, portMAX_DELAY);
        if (!s_chain_ready[role]) {
            font_rebuild_chain_locked(role);
        }
        xSemaphoreGive(s_ttf_lock);
    }
    return &s_chain[role][s_chain_idx[role]];
}

const char *font_current_name(font_role_t role)
{
    const lv_font_t *font = (role == FONT_ROLE_UI)
        ? s_font_role_primary[FONT_ROLE_UI]
        : s_font_role_primary[FONT_ROLE_TERMINAL];
    int i;

    for (i = 0; i < FONT_BUILTIN_COUNT; i++) {
        if (s_font_builtins[i].font == font) {
            return s_font_builtins[i].name;
        }
    }
    if (role == FONT_ROLE_TERMINAL || role == FONT_ROLE_UI) {
        int slot = s_role_ttf[role];
        if (slot >= 0 && slot < P4_CONFIG_FONT_TTF_SLOTS &&
            s_ttf_slots[slot].used) {
            return s_ttf_slots[slot].stem;
        }
    }
    return s_font_builtins[0].name;
}

int font_builtin_count(void)
{
    return FONT_BUILTIN_COUNT;
}

const char *font_builtin_name(int index)
{
    if (index < 0 || index >= FONT_BUILTIN_COUNT) {
        return NULL;
    }
    return s_font_builtins[index].name;
}

bool font_builtin_monospace(int index)
{
    if (index < 0 || index >= FONT_BUILTIN_COUNT) {
        return false;
    }
    return s_font_builtins[index].monospace;
}

/* Point a role at a new primary (caller holds s_ttf_lock). Releases any TTF
 * the role previously referenced and rebuilds its chain. */
static void font_apply_primary_locked(font_role_t role, const lv_font_t *primary)
{
    if (s_role_ttf[role] >= 0) {
        font_ttf_release_locked(s_role_ttf[role]);
        s_role_ttf[role] = -1;
    }
    /* The recorded primary is the source of truth for current_name() and
     * for measuring; the chain is the render copy. */
    s_font_role_primary[role] = primary;
    font_rebuild_chain_locked(role);
}

bool font_set(font_role_t role, const char *name)
{
    const font_builtin_t *found = font_find_builtin(name);

    if (role != FONT_ROLE_TERMINAL && role != FONT_ROLE_UI) {
        return false;
    }
    font_ttf_lock_init();
    xSemaphoreTake(s_ttf_lock, portMAX_DELAY);
    if (found != NULL) {
        if (role == FONT_ROLE_TERMINAL && !found->monospace) {
            xSemaphoreGive(s_ttf_lock);
            return false;
        }
        font_apply_primary_locked(role, found->font);
        font_refresh_cjk_locked(FONT_ROLE_TERMINAL);
        font_refresh_cjk_locked(FONT_ROLE_UI);
        xSemaphoreGive(s_ttf_lock);
        return true;
    }
    /* Not built-in: try an SD TTF stem at the role's current size. */
    {
        int px = s_role_px[role] > 0 ? s_role_px[role] : P4_CONFIG_FONT_DEFAULT_PX;
        int slot = font_ttf_load_locked(name, px);
        if (slot < 0) {
            xSemaphoreGive(s_ttf_lock);
            return false;
        }
        if (role == FONT_ROLE_TERMINAL && !s_ttf_slots[slot].monospace) {
            xSemaphoreGive(s_ttf_lock);
            return false;
        }
        s_ttf_slots[slot].refs++;
        font_apply_primary_locked(role, s_ttf_slots[slot].font);
        s_role_ttf[role] = slot;
        font_refresh_cjk_locked(FONT_ROLE_TERMINAL);
        font_refresh_cjk_locked(FONT_ROLE_UI);
        xSemaphoreGive(s_ttf_lock);
        return true;
    }
}

bool font_restore(const char *terminal, const char *ui)
{
    bool ok = true;

    if (terminal != NULL && !font_set(FONT_ROLE_TERMINAL, terminal)) {
        ok = false;
    }
    if (ui != NULL && !font_set(FONT_ROLE_UI, ui)) {
        ok = false;
    }
    return ok;
}

/* ========================================================================
 * SD TTF FONTS (Phase 3)
 * ======================================================================== */

/* Role pixel sizes (bitmap roles stay nominal 16). */
static int s_role_px[FONT_ROLE_COUNT];
/* Role TTF slot index (-1 = built-in primary). */
static int s_role_ttf[FONT_ROLE_COUNT];

static void font_ttf_lock_init(void)
{
    if (s_ttf_lock == NULL) {
        s_ttf_lock = xSemaphoreCreateMutex();
    }
}

/* LVGL-task deferred destroy: a replaced TTF object may still be referenced
 * by an in-flight render frame, so it is freed on the LVGL task between
 * renders, never synchronously on the worker. */
static void font_ttf_destroy_cb(void *font)
{
    int i;

    if (font == NULL) {
        return;
    }
    lv_tiny_ttf_destroy((lv_font_t *)font);
    if (s_ttf_lock != NULL) {
        xSemaphoreTake(s_ttf_lock, portMAX_DELAY);
    }
    for (i = 0; i < FONT_TTF_SLOTS; i++) {
        /* Stale check: the slot may have been reused since scheduling. */
        if (s_ttf_slots[i].used && s_ttf_slots[i].font == (lv_font_t *)font) {
            s_ttf_slots[i].used = false;
            s_ttf_slots[i].font = NULL;
            break;
        }
    }
    if (s_ttf_lock != NULL) {
        xSemaphoreGive(s_ttf_lock);
    }
}

static void font_ttf_release_locked(int slot)
{
    if (slot < 0 || slot >= FONT_TTF_SLOTS || !s_ttf_slots[slot].used) {
        return;
    }
    if (--s_ttf_slots[slot].refs <= 0) {
        s_ttf_slots[slot].refs = 0;
        /* Freed async (see above); the slot stays reserved until then. */
        lv_async_call(font_ttf_destroy_cb, s_ttf_slots[slot].font);
    }
}

/* True when every sample advance matches (monospace probe for TTFs). */
static bool font_probe_monospace(lv_font_t *font)
{
    static const char sample[] = "ilMW01 ";
    uint16_t first = 0;
    size_t i = 0;

    while (sample[i] != '\0') {
        uint16_t adv = lv_font_get_glyph_width(font, (uint32_t)(unsigned char)sample[i], 0);
        if (i == 0) {
            first = adv;
            if (first == 0) {
                return false;
            }
        } else if (adv != first) {
            return false;
        }
        i++;
    }
    return true;
}

static int font_ttf_find_locked(const char *stem, int px)
{
    int i;

    for (i = 0; i < FONT_TTF_SLOTS; i++) {
        if (s_ttf_slots[i].used && s_ttf_slots[i].px == px &&
            strcasecmp(s_ttf_slots[i].stem, stem) == 0) {
            return i;
        }
    }
    return -1;
}

/* Load (or reuse) a registry slot. Caller holds s_ttf_lock. Returns slot or -1.
 * Current fonts are untouched on failure. */
static int font_ttf_load_locked(const char *stem, int px)
{
    char host_path[P4_CONFIG_SD_PATH_BYTES];
    char lv_path[P4_CONFIG_SD_PATH_BYTES];
    const char *exts[] = { ".ttf", ".otf" };
    int slot = -1;
    int i;
    FILE *probe;
    lv_font_t *font;

    if (stem == NULL || stem[0] == '\0') {
        return -1;
    }
    slot = font_ttf_find_locked(stem, px);
    if (slot >= 0) {
        return slot;
    }
    for (i = 0; i < FONT_TTF_SLOTS; i++) {
        if (!s_ttf_slots[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        /* Pressure: evict an unreferenced slot (probe leftovers). The old
         * object frees async (in-flight renders may hold it); the slot is
         * reused immediately and the destroy callback stale-checks. */
        for (i = 0; i < FONT_TTF_SLOTS; i++) {
            if (s_ttf_slots[i].used && s_ttf_slots[i].refs <= 0) {
                ESP_LOGW(FONT_TAG, "slot pressure: evicting %s@%d",
                         s_ttf_slots[i].stem, s_ttf_slots[i].px);
                lv_async_call(font_ttf_destroy_cb, s_ttf_slots[i].font);
                slot = i;
                break;
            }
        }
    }
    if (slot < 0) {
        ESP_LOGW(FONT_TAG, "load %s: slot table full", stem);
        return -1;
    }
    /* Probe host-side first (cheap existence check, no LVGL involvement). */
    lv_path[0] = '\0';
    for (i = 0; i < 2; i++) {
        snprintf(host_path, sizeof(host_path), "%s/FONTS/%s%s",
                 BSP_SD_MOUNT_POINT, stem, exts[i]);
        probe = fopen(host_path, "rb");
        if (probe != NULL) {
            fclose(probe);
            snprintf(lv_path, sizeof(lv_path), "F:/FONTS/%s%s", stem, exts[i]);
            break;
        }
    }
    if (lv_path[0] == '\0') {
        ESP_LOGW(FONT_TAG, "load %s: not found in FONTS (%s)", stem, strerror(errno));
        return -1;
    }
    /* Creation touches LVGL heaps: hold the port lock (worker context). */
    if (!lvgl_port_lock(0)) {
        ESP_LOGW(FONT_TAG, "load %s: no port lock", stem);
        return -1;
    }
    font = lv_tiny_ttf_create_file(lv_path, px);
    if (font == NULL) {
        ESP_LOGW(FONT_TAG, "load %s: tiny_ttf create failed (%s)", stem, lv_path);
    }
    if (font != NULL) {
        s_ttf_slots[slot].used = true;
        snprintf(s_ttf_slots[slot].stem, sizeof(s_ttf_slots[slot].stem), "%s", stem);
        s_ttf_slots[slot].font = font;
        s_ttf_slots[slot].px = px;
        s_ttf_slots[slot].monospace = font_probe_monospace(font);
        s_ttf_slots[slot].refs = 0;
    }
    lvgl_port_unlock();
    return font != NULL ? slot : -1;
}

bool font_load_ttf(const char *stem)
{
    int slot;

    font_ttf_lock_init();
    xSemaphoreTake(s_ttf_lock, portMAX_DELAY);
    slot = font_ttf_load_locked(stem, P4_CONFIG_FONT_DEFAULT_PX);
    xSemaphoreGive(s_ttf_lock);
    return slot >= 0;
}

/* Refresh one role's CJK tail (caller holds s_ttf_lock). Loads
 * NotoSansSC at the role size, or clears the tail when absent. Survives SD
 * removal: the old object is async-freed (RAM only) and chains end at FA. */
static void font_refresh_cjk_locked(font_role_t role)
{
    int px = s_role_px[role] > 0 ? s_role_px[role] : P4_CONFIG_FONT_DEFAULT_PX;
    int slot = font_ttf_load_locked("NotoSansSC", px);

    if (slot == s_cjk_slot[role]) {
        return;
    }
    if (slot >= 0) {
        s_ttf_slots[slot].refs++;
    }
    if (s_cjk_slot[role] >= 0) {
        font_ttf_release_locked(s_cjk_slot[role]);
    }
    s_cjk_slot[role] = slot;
    font_ensure_fa_locked(role);
    s_fa_chain[role].fallback = slot >= 0 ? s_ttf_slots[slot].font : NULL;
}

void font_attach_cjk(void)
{
    font_ttf_lock_init();
    xSemaphoreTake(s_ttf_lock, portMAX_DELAY);
    font_refresh_cjk_locked(FONT_ROLE_TERMINAL);
    font_refresh_cjk_locked(FONT_ROLE_UI);
    xSemaphoreGive(s_ttf_lock);
}

/* ========================================================================
 * VARIANTS + SPAN STYLING
 * ======================================================================== */

/* (stem, attr) -> variant stem. Only the vendored DejaVuSansMono pair. */
static const char *font_variant_stem(const char *base_stem, int attr)
{
    if (base_stem == NULL) {
        return NULL;
    }
    if (strcasecmp(base_stem, "DejaVuSansMono") != 0) {
        return NULL;
    }
    if (attr == FONT_VARIANT_BOLD) {
        return "DejaVuSansMono-Bold";
    }
    if (attr == FONT_VARIANT_ITALIC) {
        return "DejaVuSansMono-Oblique";
    }
    return NULL;
}

lv_font_t *font_variant_for(const char *base_stem, int px, int attr)
{
    const char *stem = font_variant_stem(base_stem, attr);
    int slot;

    if (stem == NULL) {
        return NULL;
    }
    if (px < P4_CONFIG_FONT_SIZE_MIN || px > P4_CONFIG_FONT_SIZE_MAX) {
        return NULL;
    }
    font_ttf_lock_init();
    xSemaphoreTake(s_ttf_lock, portMAX_DELAY);
    slot = font_ttf_load_locked(stem, px);
    if (slot >= 0) {
        /* Permanent pin: spans hold the pointer across renders with no
         * ref path, so the slot must never be freed. Bounded by the small
         * variant set (Bold/Oblique x few sizes). */
        s_ttf_slots[slot].refs++;
    }
    xSemaphoreGive(s_ttf_lock);
    return slot >= 0 ? s_ttf_slots[slot].font : NULL;
}

void font_span_style(lv_style_t *style, font_role_t role, unsigned attrs,
                     int fg_index, uint32_t fallback_rgb)
{
    const lv_font_t *font = font_get(role);
    uint32_t rgb = fallback_rgb;

    if (style == NULL) {
        return;
    }
    if ((attrs & ANSI_ATTR_BOLD) != 0) {
        lv_font_t *variant = font_variant_for(font_current_name(role),
                                              font_current_size(role),
                                              FONT_VARIANT_BOLD);
        if (variant != NULL) {
            font = variant;
        } else if (fg_index >= 0 && fg_index < 8) {
            /* No variant (stock bitmaps): bold shows as bright. */
            rgb = ansi_get_palette_color((ansi_color_index_t)(fg_index + 8));
        }
    } else if ((attrs & ANSI_ATTR_ITALIC) != 0) {
        lv_font_t *variant = font_variant_for(font_current_name(role),
                                              font_current_size(role),
                                              FONT_VARIANT_ITALIC);
        if (variant != NULL) {
            font = variant;
        }
    }
    lv_style_set_text_font(style, font);
    lv_style_set_text_color(style, lv_color_hex(rgb));
    {
        lv_text_decor_t decor = LV_TEXT_DECOR_NONE;
        if ((attrs & ANSI_ATTR_UNDERLINE) != 0) {
            decor |= LV_TEXT_DECOR_UNDERLINE;
        }
        if ((attrs & ANSI_ATTR_STRIKE) != 0) {
            decor |= LV_TEXT_DECOR_STRIKETHROUGH;
        }
        lv_style_set_text_decor(style, decor);
    }
}

int font_scan_ttf(char out[][P4_CONFIG_FONT_NAME_BYTES], int cap)
{
    char dir_path[P4_CONFIG_SD_PATH_BYTES];
    DIR *dir;
    struct dirent *entry;
    int found = 0;

    if (cap <= 0) {
        return 0;
    }
    snprintf(dir_path, sizeof(dir_path), "%s/FONTS", BSP_SD_MOUNT_POINT);
    dir = opendir(dir_path);
    if (dir == NULL) {
        return 0;
    }
    while ((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        bool is_ttf = len > 4 && strcasecmp(entry->d_name + len - 4, ".ttf") == 0;
        bool is_otf = len > 4 && strcasecmp(entry->d_name + len - 4, ".otf") == 0;
        if ((is_ttf || is_otf) && found < cap) {
            size_t stem_len = len - 4;
            if (stem_len >= P4_CONFIG_FONT_NAME_BYTES) {
                stem_len = P4_CONFIG_FONT_NAME_BYTES - 1;
            }
            memcpy(out[found], entry->d_name, stem_len);
            out[found][stem_len] = '\0';
            found++;
        }
    }
    closedir(dir);
    return found;
}

int font_current_size(font_role_t role)
{
    if (role != FONT_ROLE_TERMINAL && role != FONT_ROLE_UI) {
        return P4_CONFIG_FONT_DEFAULT_PX;
    }
    return s_role_px[role];
}

bool font_set_size(font_role_t role, int px)
{
    int old_slot;
    int new_slot;
    const char *stem;

    if (role != FONT_ROLE_TERMINAL && role != FONT_ROLE_UI) {
        return false;
    }
    if (px < P4_CONFIG_FONT_SIZE_MIN || px > P4_CONFIG_FONT_SIZE_MAX) {
        return false;
    }
    font_ttf_lock_init();
    xSemaphoreTake(s_ttf_lock, portMAX_DELAY);
    old_slot = s_role_ttf[role];
    if (old_slot < 0) {
        /* Bitmap role font: fixed size. */
        xSemaphoreGive(s_ttf_lock);
        return false;
    }
    stem = s_ttf_slots[old_slot].stem;
    new_slot = font_ttf_load_locked(stem, px);
    if (new_slot < 0) {
        xSemaphoreGive(s_ttf_lock);
        return false;
    }
    s_ttf_slots[new_slot].refs++;
    font_apply_primary_locked(role, s_ttf_slots[new_slot].font);
    s_role_px[role] = px;
    s_role_ttf[role] = new_slot;
    font_refresh_cjk_locked(role);
    xSemaphoreGive(s_ttf_lock);
    return true;
}

bool font_is_monospace(const char *name)
{
    const font_builtin_t *found = font_find_builtin(name);
    int slot;

    if (found != NULL) {
        return found->monospace;
    }
    font_ttf_lock_init();
    xSemaphoreTake(s_ttf_lock, portMAX_DELAY);
    slot = font_ttf_load_locked(name, P4_CONFIG_FONT_DEFAULT_PX);
    xSemaphoreGive(s_ttf_lock);
    return slot >= 0 && s_ttf_slots[slot].monospace;
}

int font_terminal_max_px(const char *name, int rect_w, int rect_h)
{
    const font_builtin_t *builtin = font_find_builtin(name);
    const lv_font_t *font = NULL;
    int probe_px = P4_CONFIG_FONT_DEFAULT_PX;
    int adv_m;
    int line_h;
    int max_by_w;
    int max_by_h;
    int slot = -1;
    bool locked = false;

    if (rect_w <= 0 || rect_h <= 0) {
        return -1;
    }
    if (builtin != NULL) {
        font = builtin->font;
    } else {
        font_ttf_lock_init();
        xSemaphoreTake(s_ttf_lock, portMAX_DELAY);
        locked = true;
        slot = font_ttf_load_locked(name, probe_px);
        if (slot < 0) {
            xSemaphoreGive(s_ttf_lock);
            return -1;
        }
        font = s_ttf_slots[slot].font;
        /* Measured object stays cached in its slot (no eviction); a probe
         * for an unloaded name consumes one slot, same as a set would. */
    }
    if (!lvgl_port_lock(0)) {
        if (locked) {
            xSemaphoreGive(s_ttf_lock);
        }
        return -1;
    }
    adv_m = lv_font_get_glyph_width(font, (uint32_t)'M', 0);
    line_h = lv_font_get_line_height(font);
    lvgl_port_unlock();
    if (locked) {
        xSemaphoreGive(s_ttf_lock);
    }
    if (adv_m <= 0 || line_h <= 0) {
        return -1;
    }
    /* Linear TTF scaling: fit 80 cols x 25 rows. */
    max_by_w = (rect_w * probe_px) / (80 * adv_m);
    max_by_h = (rect_h * probe_px) / (25 * line_h);
    if (max_by_w < max_by_h) {
        return max_by_w;
    }
    return max_by_h;
}
