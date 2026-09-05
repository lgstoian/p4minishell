/**
 * @file keyboard.c
 * @brief On-screen keyboard manager implementation for P4MiniShell.
 *
 * Owns the LVGL keyboard widget and all keyboard state. Handles visibility
 * toggling with automatic UI reflow, mode switching, textarea binding,
 * and dynamic height scaling based on display resolution.
 *
 * When the keyboard is hidden, the window manager is notified via callback
 * so the transcript area can expand to fill the freed space.
 *
 * Thread safety:
 *   - All LVGL widget operations must happen on the LVGL task.
 *   - Visibility state is a simple bool (atomic on this platform).
 *   - Public API is safe to call from any task context (LVGL ops are
 *     deferred via lv_async_call where needed).
 */

#include "keyboard.h"
#include "display.h"
#include "p4minishell_config.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <inttypes.h>

/* Backward-compatibility aliases */
#define KEYBOARD_TAG                    P4_CONFIG_SHELL_TAG

/* ========================================================================
 * INTERNAL STATE
 * ======================================================================== */

/** Keyboard manager internal state. */
static struct {
    bool initialized;
    lv_obj_t *widget;               /* LVGL keyboard widget */
    lv_obj_t *textarea;             /* Bound textarea for input routing */
    bool visible;                   /* Current visibility */
    keyboard_mode_t mode;           /* Current input mode */
    void (*visibility_callback)(bool visible);  /* Called on show/hide */
    bool external_input;            /* External input mode (USB keyboard) */
    bool force_visible;             /* User override: keep visible even with external input */
} s_keyboard = {
    .initialized = false,
    .widget = NULL,
    .textarea = NULL,
    .visible = true,
    .mode = KEYBOARD_MODE_TEXT_LOWER,
    .visibility_callback = NULL,
    .external_input = false,
    .force_visible = false,
};

/* ========================================================================
 * CUSTOM KEYBOARD MAPS - FULL PRINTABLE-ASCII (0x20-0x7E) COVERAGE
 * ========================================================================
 * The LVGL default SPECIAL map omits four printable-ASCII characters that are
 * shell-critical: the pipe | (the pipe operator), the caret ^ (the shell
 * escape character), the tilde ~, and the backtick. This custom map keeps
 * every default symbol and digit and adds those four in a fourth symbol row,
 * so the on-screen keyboard can type every printable ASCII character:
 *
 *   letters a-z / A-Z   -> TEXT_LOWER / TEXT_UPPER modes (unchanged)
 *   digits 0-9          -> SPECIAL row 1 (unchanged)
 *   + & / * = % ! ? # < >  (unchanged)
 *   \ @ $ ( ) { } [ ] ; " '  (unchanged)
 *   ^ | ~ ` - _ , . :    -> NEW fourth symbol row
 *
 * The map uses the exact control-button labels LVGL's event handler matches
 * by text ("abc", LV_SYMBOL_BACKSPACE, LV_SYMBOL_NEW_LINE,
 * LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT/RIGHT, LV_SYMBOL_OK), so the built-in
 * mode switching and character routing keep working untouched.
 */
static const char * const keyboard_special_map[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "abc", "+", "&", "/", "*", "=", "%", "!", "?", "#", "<", ">", "\n",
    "\\", "@", "$", "(", ")", "{", "}", "[", "]", ";", "\"", "'", "\n",
    "^", "|", "~", "`", "-", "_", ",", ".", ":", LV_SYMBOL_NEW_LINE, "\n",
    LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, "Nav", LV_SYMBOL_OK, ""
};

static const lv_buttonmatrix_ctrl_t keyboard_special_ctrl_map[] = {
    /* Row 1: digits + backspace. */
    (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    LV_BUTTONMATRIX_CTRL_CHECKED | 2,
    /* Row 2: "abc" toggle + 11 symbols. */
    LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2,
    (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    (LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    /* Row 3: 12 symbols. */
    (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    /* Row 4: ^ | ~ ` - _ , . : + newline. */
    (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    LV_BUTTONMATRIX_CTRL_CHECKED | 2,
    /* Row 5: hide, left, space, right, nav, ok. */
    LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2,
    (LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    6,
    (LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    (LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2,
};

/* ========================================================================
 * EDITOR NAVIGATION MAPS (USER_1 = nav, USER_2 = edit nav)
 * ========================================================================
 * The editor is fully usable from the touch keyboard. Two navigation pages
 * carry every editing command:
 *
 *   Nav  (USER_1): Tab, arrows, Home/End, Del, Ins, Find, PgUp/PgDn,
 *                  Undo, Redo, Rep, Goto, Save, SaveAs, Quit, Next (find
 *                  repeat), Nav2 (-> edit page), abc (-> letters).
 *   Edit (USER_2): Copy, Cut, Paste, SelAll, WdL/WdR (word left/right),
 *                  DocH/DocE (document home/end), DelLn (delete line),
 *                  DelE (delete to end of line), Nav1 (-> nav), abc.
 *
 * These labels are routed to the editor by main.c's keyboard event callback
 * when the editor is open; the default LVGL handler leaves them alone. */

static const char * const keyboard_nav_map[] = {
    "Tab", LV_SYMBOL_UP, "Home", "Del", "Ins", "Find", "\n",
    LV_SYMBOL_LEFT, LV_SYMBOL_DOWN, LV_SYMBOL_RIGHT, "End", "PgUp", "PgDn", "\n",
    "Undo", "Redo", "Rep", "Goto", "Save", "SaveAs", "\n",
    "Quit", "Next", "Nav2", "abc", "\n",
    ""
};

static const lv_buttonmatrix_ctrl_t keyboard_nav_ctrl_map[] = {
    /* Row 1: Tab, Up, Home, Del, Ins, Find. */
    (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    /* Row 2: Left, Down, Right, End, PgUp, PgDn. */
    (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    /* Row 3: Undo, Redo, Rep, Goto, Save, SaveAs. */
    (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    /* Row 4: Quit, Next, Nav2, abc. */
    (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2,
};

static const char * const keyboard_edit_map[] = {
    "Copy", "Cut", "Paste", "SelAll", "WdL", "WdR", "\n",
    "DocH", "DocE", "DelLn", "DelE", "Nav1", "abc", "\n",
    ""
};

static const lv_buttonmatrix_ctrl_t keyboard_edit_ctrl_map[] = {
    /* Row 1: Copy, Cut, Paste, SelAll, WdL, WdR. */
    (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    /* Row 2: DocH, DocE, DelLn, DelE, Nav1, abc. */
    (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1), (LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2,
};

static void keyboard_install_custom_maps(void)
{
    if (s_keyboard.widget == NULL) {
        return;
    }

    /* Install the full-ASCII SPECIAL map. lv_keyboard_set_map() stores it per
     * mode and immediately reapplies the current mode's map, so the built-in
     * "1#"/"abc" mode buttons switch to it correctly. */
    lv_keyboard_set_map(s_keyboard.widget, LV_KEYBOARD_MODE_SPECIAL,
                        keyboard_special_map, keyboard_special_ctrl_map);

    /* Editor navigation pages (USER_1 / USER_2). */
    lv_keyboard_set_map(s_keyboard.widget, LV_KEYBOARD_MODE_USER_1,
                        keyboard_nav_map, keyboard_nav_ctrl_map);
    lv_keyboard_set_map(s_keyboard.widget, LV_KEYBOARD_MODE_USER_2,
                        keyboard_edit_map, keyboard_edit_ctrl_map);
}

/* ========================================================================
 * FORWARD DECLARATIONS
 * ======================================================================== */

static void keyboard_apply_mode(void);
static lv_keyboard_mode_t keyboard_mode_to_lvgl(keyboard_mode_t mode);

/* ========================================================================
 * MODE CONVERSION
 * ======================================================================== */

static lv_keyboard_mode_t keyboard_mode_to_lvgl(keyboard_mode_t mode)
{
    switch (mode) {
    case KEYBOARD_MODE_TEXT_LOWER: return LV_KEYBOARD_MODE_TEXT_LOWER;
    case KEYBOARD_MODE_TEXT_UPPER: return LV_KEYBOARD_MODE_TEXT_UPPER;
    case KEYBOARD_MODE_NUMBER:     return LV_KEYBOARD_MODE_NUMBER;
    case KEYBOARD_MODE_SYMBOLS:    return LV_KEYBOARD_MODE_SPECIAL;
    case KEYBOARD_MODE_NAV:        return LV_KEYBOARD_MODE_USER_1;
    case KEYBOARD_MODE_NAV2:       return LV_KEYBOARD_MODE_USER_2;
    default:                       return LV_KEYBOARD_MODE_TEXT_LOWER;
    }
}

static void keyboard_apply_mode(void)
{
    if (s_keyboard.widget == NULL) {
        return;
    }

    lv_keyboard_set_mode(s_keyboard.widget, keyboard_mode_to_lvgl(s_keyboard.mode));
}

/* ========================================================================
 * LIFECYCLE
 * ======================================================================== */

lv_obj_t *keyboard_init(lv_obj_t *parent)
{
    const lv_font_t *font;
    lv_coord_t kb_h;

    if (s_keyboard.initialized) {
        ESP_LOGW(KEYBOARD_TAG, "Keyboard already initialized; deinitializing first");
        keyboard_deinit();
    }

    if (parent == NULL) {
        ESP_LOGE(KEYBOARD_TAG, "No parent object for keyboard");
        return NULL;
    }

    /* Select terminal font */
#if LV_FONT_UNSCII_16
    font = &lv_font_unscii_16;
#else
    font = LV_FONT_DEFAULT;
#endif

    /* Calculate keyboard height from config */
    kb_h = (lv_coord_t)(display_get_height() *
                        P4_CONFIG_KEYBOARD_HEIGHT_PCT / 100);
    if (kb_h < P4_CONFIG_KEYBOARD_HEIGHT_MIN) {
        kb_h = P4_CONFIG_KEYBOARD_HEIGHT_MIN;
    }
    if (kb_h > P4_CONFIG_KEYBOARD_HEIGHT_MAX) {
        kb_h = P4_CONFIG_KEYBOARD_HEIGHT_MAX;
    }

    /* Create the LVGL keyboard widget */
    s_keyboard.widget = lv_keyboard_create(parent);
    if (s_keyboard.widget == NULL) {
        ESP_LOGE(KEYBOARD_TAG, "Failed to create keyboard widget");
        return NULL;
    }

    lv_obj_set_width(s_keyboard.widget, LV_PCT(100));
    lv_obj_set_height(s_keyboard.widget, kb_h);
    keyboard_install_custom_maps();
    lv_keyboard_set_mode(s_keyboard.widget, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_set_style_text_font(s_keyboard.widget, font, 0);
    lv_obj_set_style_bg_color(s_keyboard.widget,
                               lv_color_hex(0x1D2625), 0);
    lv_obj_set_style_bg_opa(s_keyboard.widget, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_keyboard.widget, 0, 0);

    s_keyboard.initialized = true;
    s_keyboard.visible = true;
    s_keyboard.mode = KEYBOARD_MODE_TEXT_LOWER;

    ESP_LOGI(KEYBOARD_TAG, "Keyboard initialized: height=%" PRId32 " visible=%s",
             (int32_t)kb_h, s_keyboard.visible ? "yes" : "no");

    return s_keyboard.widget;
}

void keyboard_deinit(void)
{
    if (!s_keyboard.initialized) {
        return;
    }

    /* LVGL keyboard widget is deleted by parent's lv_obj_clean() */
    s_keyboard.widget = NULL;
    s_keyboard.textarea = NULL;
    s_keyboard.visible = true;
    s_keyboard.mode = KEYBOARD_MODE_TEXT_LOWER;
    s_keyboard.visibility_callback = NULL;
    s_keyboard.initialized = false;
}

bool keyboard_is_initialized(void)
{
    return s_keyboard.initialized;
}

void keyboard_register_visibility_callback(void (*cb)(bool visible))
{
    s_keyboard.visibility_callback = cb;
}

/* ========================================================================
 * VISIBILITY
 * ======================================================================== */

void keyboard_show(void)
{
    if (!s_keyboard.initialized || s_keyboard.widget == NULL) {
        return;
    }

    if (s_keyboard.visible) {
        return; /* Already visible */
    }

    /* LVGL widget access â€” lock for safety when called from command worker
     * task (e.g. `keyboard show`/`hide`/`toggle` commands). */
    lvgl_port_lock(0);

    lv_obj_remove_flag(s_keyboard.widget, LV_OBJ_FLAG_HIDDEN);
    s_keyboard.visible = true;

    /* Re-bind textarea if one was set */
    if (s_keyboard.textarea != NULL) {
        lv_keyboard_set_textarea(s_keyboard.widget, s_keyboard.textarea);
    }

    lvgl_port_unlock();

    /* Notify window manager to reflow layout */
    if (s_keyboard.visibility_callback != NULL) {
        s_keyboard.visibility_callback(true);
    }

    ESP_LOGI(KEYBOARD_TAG, "Keyboard shown");
}

void keyboard_hide(void)
{
    if (!s_keyboard.initialized || s_keyboard.widget == NULL) {
        return;
    }

    if (!s_keyboard.visible) {
        return; /* Already hidden */
    }

    /* LVGL widget access â€” lock for safety when called from command worker
     * task (e.g. `keyboard show`/`hide`/`toggle` commands). */
    lvgl_port_lock(0);

    lv_obj_add_flag(s_keyboard.widget, LV_OBJ_FLAG_HIDDEN);
    s_keyboard.visible = false;

    lvgl_port_unlock();

    /* Notify window manager to reflow layout */
    if (s_keyboard.visibility_callback != NULL) {
        s_keyboard.visibility_callback(false);
    }

    ESP_LOGI(KEYBOARD_TAG, "Keyboard hidden");
}

void keyboard_toggle(void)
{
    if (s_keyboard.visible) {
        keyboard_hide();
    } else {
        keyboard_show();
    }
}

bool keyboard_is_visible(void)
{
    return s_keyboard.visible;
}

/* ========================================================================
 * TEXTAREA BINDING
 * ======================================================================== */

void keyboard_bind_textarea(lv_obj_t *textarea)
{
    s_keyboard.textarea = textarea;

    if (s_keyboard.widget == NULL) {
        return;
    }

    if (textarea != NULL && s_keyboard.visible) {
        lv_keyboard_set_textarea(s_keyboard.widget, textarea);
    } else if (textarea == NULL) {
        /* Truly unbind: clear the LVGL widget's textarea too, so a stray
         * handler can never type into a hidden line (the modal editor clears
         * the binding when it takes over the OSK). */
        lv_keyboard_set_textarea(s_keyboard.widget, NULL);
    }
}

lv_obj_t *keyboard_get_textarea(void)
{
    return s_keyboard.textarea;
}

bool keyboard_is_textarea_bound(void)
{
    return s_keyboard.textarea != NULL;
}

/* ========================================================================
 * OSK INPUT DEDUPLICATION
 * ======================================================================== */

bool keyboard_osk_accept_at(uint32_t btn_id, int64_t now_ms)
{
    static uint32_t s_last_id = (uint32_t)-1;
    static int64_t s_last_ms = 0;

    if (btn_id == s_last_id &&
        now_ms - s_last_ms < P4_CONFIG_OSK_DEBOUNCE_MS) {
        return false;
    }
    s_last_id = btn_id;
    s_last_ms = now_ms;
    return true;
}

bool keyboard_osk_accept(uint32_t btn_id)
{
    return keyboard_osk_accept_at(btn_id, esp_timer_get_time() / 1000);
}

/* ========================================================================
 * MODE CONTROL
 * ======================================================================== */

void keyboard_set_mode(keyboard_mode_t mode)
{
    if (mode >= KEYBOARD_MODE_COUNT) {
        return;
    }

    s_keyboard.mode = mode;
    keyboard_apply_mode();
}

keyboard_mode_t keyboard_get_mode(void)
{
    return s_keyboard.mode;
}

void keyboard_cycle_mode(void)
{
    keyboard_mode_t next = (keyboard_mode_t)((int)s_keyboard.mode + 1);
    if (next >= KEYBOARD_MODE_COUNT) {
        next = KEYBOARD_MODE_TEXT_LOWER;
    }
    keyboard_set_mode(next);
}

/* ========================================================================
 * DIMENSIONS
 * ======================================================================== */

lv_coord_t keyboard_get_height(void)
{
    lv_coord_t height;

    if (!s_keyboard.visible || s_keyboard.widget == NULL) {
        return 0;
    }

    height = lv_obj_get_height(s_keyboard.widget);

    /* The widget's height is not computed until the first LVGL layout pass,
     * so a query during windows_init() can read 0. Fall back to the configured
     * height so the transcript slot is sized correctly from the start. */
    if (height <= 0) {
        height = keyboard_get_configured_height();
    }

    return height;
}

lv_coord_t keyboard_get_configured_height(void)
{
    lv_coord_t kb_h = (lv_coord_t)(display_get_height() *
                                    P4_CONFIG_KEYBOARD_HEIGHT_PCT / 100);
    if (kb_h < P4_CONFIG_KEYBOARD_HEIGHT_MIN) {
        kb_h = P4_CONFIG_KEYBOARD_HEIGHT_MIN;
    }
    if (kb_h > P4_CONFIG_KEYBOARD_HEIGHT_MAX) {
        kb_h = P4_CONFIG_KEYBOARD_HEIGHT_MAX;
    }
    return kb_h;
}

/* ========================================================================
 * EXTERNAL INPUT MODE (USB KEYBOARD)
 * ======================================================================== */

void keyboard_set_external_input(bool enabled)
{
    s_keyboard.external_input = enabled;
    s_keyboard.force_visible = false;  /* Reset force override on mode change */

    if (enabled && s_keyboard.visible && !s_keyboard.force_visible) {
        /* External input enabled: auto-hide on-screen keyboard */
        keyboard_hide();
        ESP_LOGI(KEYBOARD_TAG, "External input enabled; keyboard auto-hidden");
    } else if (!enabled && !s_keyboard.visible) {
        /* External input disabled: restore on-screen keyboard */
        keyboard_show();
        ESP_LOGI(KEYBOARD_TAG, "External input disabled; keyboard restored");
    }
}

bool keyboard_is_external_input_enabled(void)
{
    return s_keyboard.external_input;
}

void keyboard_force_visible(void)
{
    s_keyboard.force_visible = true;
    if (!s_keyboard.visible) {
        keyboard_show();
    }
}

void keyboard_clear_force_visible(void)
{
    s_keyboard.force_visible = false;
    /* Re-evaluate: if external input is active, hide the keyboard */
    if (s_keyboard.external_input && s_keyboard.visible) {
        keyboard_hide();
    }
}

/* ========================================================================
 * WIDGET ACCESS
 * ======================================================================== */

lv_obj_t *keyboard_get_widget(void)
{
    return s_keyboard.widget;
}

/* ========================================================================
 * LVGL EVENT CALLBACK
 * ======================================================================== */

/**
 * Remove every LVGL event callback from the keyboard widget.
 *
 * lv_obj_remove_event_cb(obj, NULL) is a NO-OP in LVGL (it only removes
 * callbacks whose cb pointer equals NULL), so it cannot strip the LVGL
 * default keyboard handler. Removing descriptors explicitly guarantees the
 * widget ends up with exactly the callbacks this module registers.
 */
static void keyboard_remove_all_callbacks(void)
{
    if (s_keyboard.widget == NULL) {
        return;
    }
    while (lv_obj_get_event_count(s_keyboard.widget) > 0) {
        lv_event_dsc_t *dsc = lv_obj_get_event_dsc(s_keyboard.widget, 0);
        if (dsc == NULL) {
            break;
        }
        lv_obj_remove_event_dsc(s_keyboard.widget, dsc);
    }
}

uint32_t keyboard_event_callback_count(void)
{
    return s_keyboard.widget != NULL ? lv_obj_get_event_count(s_keyboard.widget) : 0;
}

void keyboard_register_event_callback(lv_event_cb_t cb, void *user_data)
{
    if (s_keyboard.widget == NULL) {
        ESP_LOGW(KEYBOARD_TAG, "Cannot register event callback: keyboard not initialized");
        return;
    }

    /* Remove the LVGL default handler AND any previously registered callback
     * so this callback is the widget's sole LV_EVENT_VALUE_CHANGED handler
     * (a surviving default handler was the cause of double OSK input). */
    keyboard_remove_all_callbacks();

    if (cb != NULL) {
        lv_obj_add_event_cb(s_keyboard.widget, cb, LV_EVENT_ALL, user_data);
        ESP_LOGI(KEYBOARD_TAG, "Keyboard event callback registered");
    }

    /* Audit: exactly one handler should remain. Any other count means a
     * duplicate was (re)registered and double input would follow. A mismatch
     * is a real problem (warn); the healthy state stays quiet (info). */
    {
        uint32_t count = keyboard_event_callback_count();
        if (count == 1) {
            ESP_LOGI(KEYBOARD_TAG, "Keyboard callback audit: 1 handler registered");
        } else {
            ESP_LOGW(KEYBOARD_TAG,
                     "Keyboard callback audit: %" PRIu32 " handler(s) registered "
                     "(expected 1) - DOUBLE INPUT RISK",
                     count);
        }
    }
}
