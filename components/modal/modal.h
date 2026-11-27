/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file modal.h
 * @module modal
 * @layer leaf
 * @domain ui
 * @provides shared modal runtime + surfaces (dialog/list/ask/view)
 * @status stable
 * @test test/main/test_modal.c
 * @hard-rule ai-context.md#module-layering-rules
 * @brief Shared modal-surface runtime for P4MiniShell.
 *
 * The Hybrid route makes batch files the apps and native code the polished
 * screens. A batch app launches a small native modal surface that takes over
 * the current shell display area (the transcript container) when it needs a
 * real UI. The runtime generalises the editor pattern: a worker-side session
 * loop, an LVGL view opened via lv_async_call, input capture through the shell
 * ops table, and a graceful close handoff. Surfaces live in their own
 * components (editor, dialog, list, ask) and plug into this runtime so the
 * lifecycle lives exactly once.
 */

#ifndef P4MINISHELL_MODAL_H
#define P4MINISHELL_MODAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Generic event bits managed by the runtime. Surfaces may use BIT2+. */
#define MODAL_EVENT_CLOSE_REQUEST   BIT0
#define MODAL_EVENT_CLOSED          BIT1

/* Optional async bits used by the ready-made surfaces. Only one surface is
 * ever active on the ONE shared runtime, so they may reuse BIT2 by contract.
 * `focus` is set by a touch/console callback, `progress_*` by a timer or a
 * native publisher; the surface's `service()` repaints on the worker task. */
#define MODAL_EVENT_FOCUS_CHANGED   BIT2
#define MODAL_EVENT_PROGRESS_TICK   BIT2
#define MODAL_EVENT_PROGRESS_DONE   BIT3

/* Forward declaration of the surface descriptor. */
typedef struct modal_surface modal_surface_t;

/**
 * @brief One native modal surface type.
 *
 * The surface describes how to open/close itself and how to handle input.
 * The runtime owns the event group and the session loop; the surface uses the
 * event group to signal close and any custom events.
 */
struct modal_surface {
    const char *name;

    /**
     * @brief Called on the worker task to start the surface.
     *
     * The surface should schedule its LVGL view open via lv_async_call and
     * return true on success. @p event_group is owned by the runtime and is
     * safe to use from the LVGL task and the worker task.
     */
    bool (*open)(void *ctx, EventGroupHandle_t event_group);

    /**
     * @brief Called on the worker task when event bits are set.
     *
     * Custom bits (BIT2+) are passed through. The runtime handles
     * MODAL_EVENT_CLOSE_REQUEST internally; surfaces may ignore it.
     */
    void (*service)(void *ctx, EventBits_t bits);

    /**
     * @brief Called on the worker task to close the surface.
     *
     * The surface should schedule its LVGL view close and eventually set
     * MODAL_EVENT_CLOSED in the event group.
     */
    void (*close)(void *ctx);

    /**
     * @brief Handle a USB key while the surface is active.
     * @return true if the key was consumed.
     */
    bool (*handle_usb_key)(void *ctx, uint8_t key_code, uint8_t modifiers, char ascii);

    /**
     * @brief Handle a serial console line while the surface is active.
     * @return true if the line was consumed.
     */
    bool (*handle_serial_line)(void *ctx, const char *line);

    /**
     * @brief Called on the worker task when the display layout changed
     * (rotation / resolution), so the surface can re-apply its panel geometry
     * in place instead of being torn down. NULL when the surface needs none.
     */
    void (*reflow)(void *ctx);

    /**
     * @brief When true, a foreground break (Ctrl+C / Stop) cancels the surface.
     *
     * Ready-made input/pick surfaces (dialog/list/ask/form/browse/view/…) set
     * this so an app can never wedge the worker. The editor leaves it false:
     * it owns a quit confirmation and must not be force-closed (unsaved data).
     */
    bool abort_cancels;
};

/**
 * @brief Run a modal surface on the command worker task.
 *
 * Blocks until the surface closes, then returns its errorlevel. The surface
 * takes over the transcript region via windows_enter_editor_mode() and
 * captures all input sources.
 *
 * @param surface       Surface type descriptor.
 * @param ctx           Surface instance context (owned by caller).
 * @param errorlevel    Receives the command errorlevel (0 on clean exit).
 * @return ESP_OK on success, or an error code on failure to start.
 */
esp_err_t modal_surface_run(const modal_surface_t *surface, void *ctx, int *errorlevel);

/** @return true when any modal surface is currently active. */
bool modal_runtime_is_active(void);

/** @return the name of the active surface, or NULL. */
const char *modal_runtime_active_name(void);

/** Route a USB key to the active surface. @return true if consumed. */
bool modal_runtime_handle_usb_key(uint8_t key_code, uint8_t modifiers, char ascii);

/** Route a serial line to the active surface. @return true if consumed. */
bool modal_runtime_handle_serial_line(const char *line);

/**
 * @brief Signal the active surface from a timer or another task.
 *
 * Sets @p bits on the event group owned by the currently running modal
 * session. This is the single async channel for the ready-made surfaces
 * (picker focus repaint, progress ticks/completion). No new task or input
 * path is created: the bits are serviced by the same session loop that owns
 * the shell_command_ops_t.modal_* hooks.
 *
 * @return true when a surface was active and the bits were set.
 */
bool modal_runtime_signal(EventBits_t bits);

/* -------------------------------------------------------------------------
 * SUSPENDED SESSION STACK (app switching on the one runtime)
 * -------------------------------------------------------------------------
 * A snapshot of a modal app: its surface/context, a saved transcript, the
 * alpha pair in force and the focused row. Only one surface runs at a time;
 * the stack is pure save/restore bookkeeping around that single session, so
 * there is no second scheduler and no second input path. Inspired by the
 * layered app-switching ideas of BMDS / ESP-Brookesia (credit only) - the
 * implementation below is expressed with our own shell app-mode primitives.
 */

/** Push a session snapshot; saves the current screen via shell_screen_save()
 *  and stores the current alpha pair. @p surface/@p ctx may be NULL for a
 *  metadata-only snapshot used by the batch verb family. */
bool modal_session_push(const char *name, const modal_surface_t *surface, void *ctx);

/** Pop the top snapshot, discard its screen and restore the one underneath. */
bool modal_session_pop(void);

/** Mark the top session suspended / resume it (state-only transitions). */
bool modal_session_suspend(void);
bool modal_session_resume(void);

/** Bring a named session to the top; restores its screen + alpha pair. */
bool modal_session_switch(const char *name);

/**
 * @brief Cycle the foreground app session by @p step (+1 forward, -1 back).
 *
 * The single session-switch helper shared by Alt+Tab (modal input hook) and
 * the TUI bottom-left edge swipe. It saves the outgoing screen/alpha through
 * shell_screen_save(), rotates the EXISTING mstack with
 * modal_session_stack_switch(), keeps the parallel screen/context arrays in
 * step, then restores the incoming screen. No task, scheduler or input path
 * is created. Returns false when the stack is empty or the switch fails.
 */
bool modal_session_cycle(int step);

/** Frame name at @p from_top (0 = top) for the app switcher view. */
const char *modal_session_name_at(int from_top);

/**
 * @brief Install the one app-switcher view callback (NULL clears it).
 *
 * main registers the TUI renderer once at boot. Each successful cycle calls
 * it with the stack in from-top order, so the overlay is drawn by the ONE tui
 * frame renderer. No modal -> tui build dependency is introduced.
 */
void modal_set_app_switcher_view(void (*view)(const char *const *names,
                                              int count, int focus));

int  modal_session_depth(void);
const char *modal_session_top_name(void);
int  modal_session_find(const char *name);
bool modal_session_focus_set(int focus);
int  modal_session_focus_get(void);

/** Drop every snapshot (discarding saved screens), leaving the stack empty. */
void modal_session_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_MODAL_H */
