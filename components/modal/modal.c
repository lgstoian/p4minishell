/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file modal.c
 * @brief Shared modal-surface runtime.
 *
 * One session loop + input-routing layer for every native modal surface that
 * a batch app can launch into the current shell display area.
 */

#include "modal.h"
#include "modal_surf.h"

#include "shell.h"
#include "windows.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "p4heap.h"
#include "esp_log.h"
#include "p4minishell_config.h"

#ifndef MODAL_TAG
#define MODAL_TAG "modal"
#endif

static const modal_surface_t *s_active_surface = NULL;
static void *s_active_ctx = NULL;
static EventGroupHandle_t s_active_eg = NULL;
static SemaphoreHandle_t s_modal_mutex = NULL;

/* The one app-switcher view, installed by main at boot. modal never includes
 * a tui header: the registration seam keeps the leaf modules unlinked at
 * build time while both inputs still call the same switch helper below. */
static void (*s_switcher_view)(const char *const *names, int count, int focus) = NULL;

static bool modal_runtime_lock(void)
{
    if (s_modal_mutex == NULL) {
        s_modal_mutex = xSemaphoreCreateMutex();
    }
    if (s_modal_mutex == NULL) {
        return false;
    }
    xSemaphoreTake(s_modal_mutex, portMAX_DELAY);
    return true;
}

static void modal_runtime_unlock(void)
{
    if (s_modal_mutex != NULL) {
        xSemaphoreGive(s_modal_mutex);
    }
}

bool modal_runtime_is_active(void)
{
    bool active;
    if (!modal_runtime_lock()) {
        return false;
    }
    active = (s_active_surface != NULL);
    modal_runtime_unlock();
    return active;
}

const char *modal_runtime_active_name(void)
{
    const char *name = NULL;
    if (!modal_runtime_lock()) {
        return NULL;
    }
    if (s_active_surface != NULL) {
        name = s_active_surface->name;
    }
    modal_runtime_unlock();
    return name;
}

static void modal_runtime_set_active(const modal_surface_t *surface, void *ctx)
{
    if (modal_runtime_lock()) {
        s_active_surface = surface;
        s_active_ctx = ctx;
        s_active_eg = NULL;   /* set once the session event group exists */
        modal_runtime_unlock();
    }
}

static void modal_runtime_clear_active(void)
{
    if (modal_runtime_lock()) {
        s_active_surface = NULL;
        s_active_ctx = NULL;
        s_active_eg = NULL;
        modal_runtime_unlock();
    }
}

bool modal_runtime_signal(EventBits_t bits)
{
    bool signalled = false;

    if (!modal_runtime_lock()) {
        return false;
    }
    if (s_active_eg != NULL) {
        xEventGroupSetBits(s_active_eg, bits);
        signalled = true;
    }
    modal_runtime_unlock();
    return signalled;
}

bool modal_runtime_handle_usb_key(uint8_t key_code, uint8_t modifiers, char ascii)
{
    bool consumed = false;
    bool switch_app = false;

    if (!modal_runtime_lock()) {
        return false;
    }
    if (s_active_surface != NULL) {
        /* Alt+Tab is a GLOBAL app-switch chord: it must never leak into the
         * active surface's own Ctrl/ASCII vocabulary. All three keyboard
         * sources (USB HID, BLE, Tab5Keyboard) reach this hook through the
         * shell input path, so there is no second key driver. The cycle runs
         * after unlocking: modal_session_cycle() takes the same mutex. */
        if (modal_alt_tab(key_code, modifiers)) {
            switch_app = true;
        } else if (s_active_surface->handle_usb_key != NULL) {
            consumed = s_active_surface->handle_usb_key(s_active_ctx, key_code, modifiers, ascii);
        }
    }
    modal_runtime_unlock();
    if (switch_app) {
        return modal_session_cycle(1);
    }
    return consumed;
}

bool modal_runtime_handle_serial_line(const char *line)
{
    bool consumed = false;
    if (!modal_runtime_lock()) {
        return false;
    }
    if (s_active_surface != NULL && s_active_surface->handle_serial_line != NULL) {
        consumed = s_active_surface->handle_serial_line(s_active_ctx, line);
    }
    modal_runtime_unlock();
    return consumed;
}

/* Aliases expected by shell_command_ops_t */
bool modal_is_active(void)
{
    return modal_runtime_is_active();
}

bool modal_handle_usb_key(uint8_t key_code, uint8_t modifiers, char ascii)
{
    return modal_runtime_handle_usb_key(key_code, modifiers, ascii);
}

bool modal_handle_serial_line(const char *line)
{
    return modal_runtime_handle_serial_line(line);
}

esp_err_t modal_surface_run(const modal_surface_t *surface, void *ctx, int *errorlevel)
{
    EventGroupHandle_t event_group = NULL;
    StaticEventGroup_t *eg_buf = NULL;
    bool opened = false;
    esp_err_t result = ESP_OK;
    EventBits_t bits;
    uint32_t layout_gen = windows_layout_generation();

    if (surface == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Background workers are headless: opening a modal (dialog/list/ask/
     * editor/viewer) would fight the main task for the single screen and
     * key queue, so refuse fast with an errorlevel the caller reports. */
    if (shell_is_background_task()) {
        if (errorlevel != NULL) {
            *errorlevel = 1;
        }
        return ESP_ERR_NOT_FINISHED;
    }
    if (errorlevel != NULL) {
        *errorlevel = 0;
    }

    /* Prefer PSRAM for the session event group so modal sessions do not
     * fragment the internal DMA-capable heap shared with LVGL spans and SD
     * DMA (M21); fall back to the internal heap when PSRAM is unavailable.
     * A statically-created group never frees its buffer on delete, so the
     * PSRAM block is tracked here and released on both exit paths. */
    eg_buf = p4heap_alloc_any(sizeof(*eg_buf));
    if (eg_buf != NULL) {
        event_group = xEventGroupCreateStatic(eg_buf);
        if (event_group == NULL) {
            heap_caps_free(eg_buf);
            eg_buf = NULL;
        }
    }
    if (event_group == NULL) {
        event_group = xEventGroupCreate();
    }
    if (event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    modal_runtime_set_active(surface, ctx);
    if (modal_runtime_lock()) {
        s_active_eg = event_group;
        modal_runtime_unlock();
    }

    if (surface->open != NULL) {
        opened = surface->open(ctx, event_group);
    } else {
        opened = true;
    }

    if (!opened) {
        modal_runtime_clear_active();
        vEventGroupDelete(event_group);
        heap_caps_free(eg_buf);
        if (errorlevel != NULL) {
            *errorlevel = 1;
        }
        return ESP_FAIL;
    }

    /* Service events until the surface asks to close. Custom bits are
     * serviced before checking CLOSE_REQUEST so a final save/confirm is not
     * dropped when the user quits in the same instant. The wait is bounded so
     * a foreground break can be polled: an app that never closes its surface
     * must not be able to wedge the command worker (the request stays set so
     * the batch line loop still unwinds the script with `^C`). */
    for (;;) {
        bits = xEventGroupWaitBits(event_group,
                                   MODAL_EVENT_CLOSE_REQUEST | MODAL_EVENT_CLOSED | 0x00FFFFFC,
                                   pdTRUE, pdFALSE, pdMS_TO_TICKS(100));

        if (bits != 0 && surface->service != NULL) {
            surface->service(ctx, bits);
        }

        /* A rotation/resolution change (in-place windows reflow) bumps the
         * window manager's layout generation. Re-apply this surface's panel
         * geometry instead of dying with the old one. */
        if (surface->reflow != NULL) {
            uint32_t now = windows_layout_generation();

            if (now != layout_gen) {
                layout_gen = now;
                surface->reflow(ctx);
            }
        }

        if ((bits & MODAL_EVENT_CLOSE_REQUEST) != 0) {
            break;
        }
        if (surface->abort_cancels && shell_abort_requested()) {
            break;
        }
    }

    modal_runtime_clear_active();

    /* Ask the surface to tear down its LVGL view and wait for confirmation. */
    if (surface->close != NULL) {
        surface->close(ctx);
    }

    {
        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
        while ((xEventGroupGetBits(event_group) & MODAL_EVENT_CLOSED) == 0 &&
               xTaskGetTickCount() < deadline) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    vEventGroupDelete(event_group);
    heap_caps_free(eg_buf);

    if (result != ESP_OK && errorlevel != NULL && *errorlevel == 0) {
        *errorlevel = 1;
    }

    return result;
}

/* =========================================================================
 * SUSPENDED SESSION STACK
 * =========================================================================
 * The pure state machine lives in modal_args.c; this layer adds only the
 * hardware-facing parts: a transcript snapshot through shell_screen_save(),
 * alpha capture through the ONE modal_surf_set_alpha() setting, and the
 * surface/context pointers that a batch app associates with each slot. No
 * task, scheduler or input path is created - switching happens at surface
 * boundaries, driven by the thin `mstack` verb family.
 */
typedef struct {
    modal_session_stack_t st;
    /* Names are copied: a batch verb's argv buffer does not outlive the
     * command, but a suspended session does. */
    char names[MODAL_SESSION_MAX][32];
    const modal_surface_t *surface[MODAL_SESSION_MAX];
    void *ctx[MODAL_SESSION_MAX];
    char *screen[MODAL_SESSION_MAX];
    uint8_t panel_alpha[MODAL_SESSION_MAX];
    uint8_t scrim_alpha[MODAL_SESSION_MAX];
} modal_session_store_t;

static modal_session_store_t s_sessions;

/** Physical index of the slot @p from_top levels below the top. */
static int session_phys(int from_top)
{
    return s_sessions.st.depth - 1 - from_top;
}

/** Save the outgoing top's screen + alpha into its own slot. */
static void session_save_outgoing(void)
{
    int idx;

    if (s_sessions.st.depth <= 0) {
        return;
    }
    idx = s_sessions.st.depth - 1;
    if (s_sessions.screen[idx] == NULL) {
        s_sessions.screen[idx] = shell_screen_save();
    }
    modal_surf_get_alpha(&s_sessions.panel_alpha[idx],
                         &s_sessions.scrim_alpha[idx]);
}

/** Restore a slot's screen + alpha when it becomes foreground again. */
static void session_restore(int from_top)
{
    int idx = session_phys(from_top);

    if (idx < 0 || idx >= MODAL_SESSION_MAX) {
        return;
    }
    if (s_sessions.screen[idx] != NULL) {
        shell_screen_restore(s_sessions.screen[idx]);
    }
    modal_surf_set_alpha(s_sessions.panel_alpha[idx],
                         s_sessions.scrim_alpha[idx]);
}

/**
 * Keep the parallel screen/context arrays aligned with the slot rotation
 * performed by modal_session_stack_switch(). The pure stack rotates its own
 * (name/focus/state) structs left from @p from_top; the hardware-facing
 * payload follows the same left-shift so each name keeps its own screen,
 * surface, context and alpha pair. No new state store is introduced.
 */
static void session_metadata_rotate(int from_top)
{
    int depth = s_sessions.st.depth;
    int idx = depth - 1 - from_top;
    int i;

    if (idx < 0 || idx >= depth - 1) {
        return;
    }
    for (i = idx; i < depth - 1; i++) {
        const modal_surface_t *surface = s_sessions.surface[i];
        void *ctx = s_sessions.ctx[i];
        char *screen = s_sessions.screen[i];
        uint8_t panel = s_sessions.panel_alpha[i];
        uint8_t scrim = s_sessions.scrim_alpha[i];

        s_sessions.surface[i] = s_sessions.surface[i + 1];
        s_sessions.ctx[i] = s_sessions.ctx[i + 1];
        s_sessions.screen[i] = s_sessions.screen[i + 1];
        s_sessions.panel_alpha[i] = s_sessions.panel_alpha[i + 1];
        s_sessions.scrim_alpha[i] = s_sessions.scrim_alpha[i + 1];

        s_sessions.surface[i + 1] = surface;
        s_sessions.ctx[i + 1] = ctx;
        s_sessions.screen[i + 1] = screen;
        s_sessions.panel_alpha[i + 1] = panel;
        s_sessions.scrim_alpha[i + 1] = scrim;
    }
}

/** Publish a stack snapshot (collected under the lock) to the one view.
 *  The view runs AFTER unlocking: tui_flush takes the LVGL lock, so holding
 *  the modal mutex across it would invert the two lock orders. */
static void session_publish_view(const char *const *names, int count)
{
    if (s_switcher_view != NULL) {
        s_switcher_view(names, count, 0);
    }
}

/** Copy the from-top name order into a caller-owned array (under lock). */
static void session_collect_names(const char **names, int *count_out)
{
    int count = s_sessions.st.depth;
    int i;

    for (i = 0; i < count; i++) {
        names[i] = modal_session_stack_name(&s_sessions.st, i);
    }
    *count_out = count;
}

bool modal_session_push(const char *name, const modal_surface_t *surface, void *ctx)
{
    bool ok = false;
    int idx;

    if (name == NULL || name[0] == '\0') {
        return false;
    }
    if (!modal_runtime_lock()) {
        return false;
    }
    if (modal_session_stack_full(&s_sessions.st)) {
        modal_runtime_unlock();
        return false;
    }
    session_save_outgoing();
    idx = s_sessions.st.depth;
    snprintf(s_sessions.names[idx], sizeof(s_sessions.names[idx]),
             "%s", name);
    ok = modal_session_stack_push(&s_sessions.st, s_sessions.names[idx]);
    if (ok) {
        idx = s_sessions.st.depth - 1;
        s_sessions.surface[idx] = surface;
        s_sessions.ctx[idx] = ctx;
        s_sessions.screen[idx] = NULL;
        modal_surf_get_alpha(&s_sessions.panel_alpha[idx],
                             &s_sessions.scrim_alpha[idx]);
    }
    modal_runtime_unlock();
    return ok;
}

bool modal_session_pop(void)
{
    bool ok = false;
    char *discard = NULL;

    if (!modal_runtime_lock()) {
        return false;
    }
    if (s_sessions.st.depth > 0) {
        int idx = s_sessions.st.depth - 1;
        discard = s_sessions.screen[idx];
        s_sessions.screen[idx] = NULL;
        s_sessions.surface[idx] = NULL;
        s_sessions.ctx[idx] = NULL;
    }
    ok = modal_session_stack_pop(&s_sessions.st);
    if (ok) {
        session_restore(0);
    }
    modal_runtime_unlock();
    shell_screen_discard(discard);
    return ok;
}

bool modal_session_suspend(void)
{
    bool ok;

    if (!modal_runtime_lock()) {
        return false;
    }
    session_save_outgoing();
    ok = modal_session_stack_suspend(&s_sessions.st);
    modal_runtime_unlock();
    return ok;
}

bool modal_session_resume(void)
{
    bool ok;

    if (!modal_runtime_lock()) {
        return false;
    }
    ok = modal_session_stack_resume(&s_sessions.st);
    if (ok) {
        session_restore(0);
    }
    modal_runtime_unlock();
    return ok;
}

bool modal_session_switch(const char *name)
{
    bool ok = false;
    int from_top;
    const char *names[MODAL_SESSION_MAX];
    int count = 0;

    if (!modal_runtime_lock()) {
        return false;
    }
    from_top = modal_session_stack_find(&s_sessions.st, name);
    session_save_outgoing();
    ok = modal_session_stack_switch(&s_sessions.st, name);
    if (ok) {
        session_metadata_rotate(from_top);
        session_restore(0);
        session_collect_names(names, &count);
    }
    modal_runtime_unlock();
    if (ok) {
        session_publish_view(names, count);
    }
    return ok;
}

bool modal_session_cycle(int step)
{
    bool ok = false;
    int from_top;
    const char *name;
    const char *names[MODAL_SESSION_MAX];
    int count = 0;

    if (!modal_runtime_lock()) {
        return false;
    }
    from_top = modal_session_next_index(&s_sessions.st, step);
    name = modal_session_stack_name(&s_sessions.st, from_top);
    if (name == NULL) {
        modal_runtime_unlock();
        return false;
    }
    session_save_outgoing();
    ok = modal_session_stack_switch(&s_sessions.st, name);
    if (ok) {
        session_metadata_rotate(from_top);
        session_restore(0);
        session_collect_names(names, &count);
    }
    modal_runtime_unlock();
    if (ok) {
        session_publish_view(names, count);
    }
    return ok;
}

const char *modal_session_name_at(int from_top)
{
    const char *name;

    if (!modal_runtime_lock()) {
        return NULL;
    }
    name = modal_session_stack_name(&s_sessions.st, from_top);
    modal_runtime_unlock();
    return name;
}

void modal_set_app_switcher_view(void (*view)(const char *const *names,
                                              int count, int focus))
{
    if (!modal_runtime_lock()) {
        return;
    }
    s_switcher_view = view;
    modal_runtime_unlock();
}

int modal_session_depth(void)
{
    int depth;

    if (!modal_runtime_lock()) {
        return 0;
    }
    depth = modal_session_stack_depth(&s_sessions.st);
    modal_runtime_unlock();
    return depth;
}

const char *modal_session_top_name(void)
{
    const char *name = NULL;

    if (!modal_runtime_lock()) {
        return NULL;
    }
    name = modal_session_stack_name(&s_sessions.st, 0);
    modal_runtime_unlock();
    return name;
}

int modal_session_find(const char *name)
{
    int found;

    if (!modal_runtime_lock()) {
        return -1;
    }
    found = modal_session_stack_find(&s_sessions.st, name);
    modal_runtime_unlock();
    return found;
}

bool modal_session_focus_set(int focus)
{
    bool ok;

    if (!modal_runtime_lock()) {
        return false;
    }
    ok = modal_session_stack_set_focus(&s_sessions.st, 0, focus);
    modal_runtime_unlock();
    return ok;
}

int modal_session_focus_get(void)
{
    int focus;

    if (!modal_runtime_lock()) {
        return -1;
    }
    focus = modal_session_stack_focus(&s_sessions.st, 0);
    modal_runtime_unlock();
    return focus;
}

void modal_session_clear(void)
{
    int i;

    if (!modal_runtime_lock()) {
        return;
    }
    for (i = 0; i < MODAL_SESSION_MAX; i++) {
        if (s_sessions.screen[i] != NULL) {
            shell_screen_discard(s_sessions.screen[i]);
            s_sessions.screen[i] = NULL;
        }
        s_sessions.surface[i] = NULL;
        s_sessions.ctx[i] = NULL;
    }
    modal_session_stack_init(&s_sessions.st);
    modal_runtime_unlock();
}
