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
#include <string.h>

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

    /* LVGL widget access — lock for safety when called from command worker
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

    /* LVGL widget access — lock for safety when called from command worker
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

    if (s_keyboard.widget != NULL && textarea != NULL && s_keyboard.visible) {
        lv_keyboard_set_textarea(s_keyboard.widget, textarea);
    }
}

lv_obj_t *keyboard_get_textarea(void)
{
    return s_keyboard.textarea;
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
    if (!s_keyboard.visible || s_keyboard.widget == NULL) {
        return 0;
    }

    return lv_obj_get_height(s_keyboard.widget);
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

void keyboard_register_event_callback(lv_event_cb_t cb, void *user_data)
{
    if (s_keyboard.widget == NULL) {
        ESP_LOGW(KEYBOARD_TAG, "Cannot register event callback: keyboard not initialized");
        return;
    }

    /* Remove any previously registered callback to avoid duplicates */
    lv_obj_remove_event_cb(s_keyboard.widget, NULL);

    if (cb != NULL) {
        lv_obj_add_event_cb(s_keyboard.widget, cb, LV_EVENT_ALL, user_data);
        ESP_LOGI(KEYBOARD_TAG, "Keyboard event callback registered");
    }
}
