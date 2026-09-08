/**
 * @file modal.c
 * @brief Shared modal-surface runtime.
 *
 * One session loop + input-routing layer for every native modal surface that
 * a batch app can launch into the current shell display area.
 */

#include "modal.h"

#include <stdlib.h>
#include <string.h>
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "p4minishell_config.h"

#ifndef MODAL_TAG
#define MODAL_TAG "modal"
#endif

static const modal_surface_t *s_active_surface = NULL;
static void *s_active_ctx = NULL;
static SemaphoreHandle_t s_modal_mutex = NULL;

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
        modal_runtime_unlock();
    }
}

static void modal_runtime_clear_active(void)
{
    if (modal_runtime_lock()) {
        s_active_surface = NULL;
        s_active_ctx = NULL;
        modal_runtime_unlock();
    }
}

bool modal_runtime_handle_usb_key(uint8_t key_code, uint8_t modifiers, char ascii)
{
    bool consumed = false;
    if (!modal_runtime_lock()) {
        return false;
    }
    if (s_active_surface != NULL && s_active_surface->handle_usb_key != NULL) {
        consumed = s_active_surface->handle_usb_key(s_active_ctx, key_code, modifiers, ascii);
    }
    modal_runtime_unlock();
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

    if (surface == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (errorlevel != NULL) {
        *errorlevel = 0;
    }

    /* Prefer PSRAM for the session event group so modal sessions do not
     * fragment the internal DMA-capable heap shared with LVGL spans and SD
     * DMA (M21); fall back to the internal heap when PSRAM is unavailable.
     * A statically-created group never frees its buffer on delete, so the
     * PSRAM block is tracked here and released on both exit paths. */
    eg_buf = heap_caps_malloc(sizeof(*eg_buf), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
     * dropped when the user quits in the same instant. */
    for (;;) {
        bits = xEventGroupWaitBits(event_group,
                                   MODAL_EVENT_CLOSE_REQUEST | MODAL_EVENT_CLOSED | 0x00FFFFFC,
                                   pdTRUE, pdFALSE, portMAX_DELAY);

        if (surface->service != NULL) {
            surface->service(ctx, bits);
        }

        if ((bits & MODAL_EVENT_CLOSE_REQUEST) != 0) {
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
