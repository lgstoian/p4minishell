/**
 * @file modal.h
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

#ifdef __cplusplus
}
#endif

#endif /* P4MINISHELL_MODAL_H */
