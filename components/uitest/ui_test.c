/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
/**
 * @file ui_test.c
 * @brief Synthetic touch injection for firmware UI automation (see ui_test.h).
 */

#include "ui_test.h"

#include "display.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "lvgl.h"

#include <string.h>

#define UI_TEST_TAG            "uitest"
#define UI_TEST_MAX_STEPS      64    /* swipe steps; kept small (internal BSS) */
#define UI_TEST_SCRIPT_PERIOD  5     /* ms between script steps */
#define UI_TEST_SETTLE_MS      70    /* extra time after the last step */
#define UI_TEST_DONE_BIT       0x1

typedef struct {
    int x;
    int y;
    bool pressed;
    uint32_t at_ms;
} ui_step_t;

static struct {
    bool ready;
    lv_indev_t *indev;
    lv_timer_t *script_timer;
    SemaphoreHandle_t lock;
    EventGroupHandle_t done;

    /* Current synthetic pointer state (read by the indev read callback). */
    volatile int cur_x;
    volatile int cur_y;
    volatile bool cur_pressed;

    /* Active script. */
    bool running;
    ui_step_t steps[UI_TEST_MAX_STEPS];
    int step_count;
    int step_idx;
    uint32_t start_tick;
    uint32_t end_ms;
} s_ui;

/* ========================================================================
 * POINTER STATE
 * ======================================================================== */

static void ui_test_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point.x = (lv_coord_t)s_ui.cur_x;
    data->point.y = (lv_coord_t)s_ui.cur_y;
    data->state = s_ui.cur_pressed ? LV_INDEV_STATE_PRESSED
                                   : LV_INDEV_STATE_RELEASED;
}

static int ui_test_clamp_x(int x)
{
    int32_t w = display_get_width();
    if (x < 0) {
        x = 0;
    }
    if (w > 0 && x >= w) {
        x = (int)w - 1;
    }
    return x;
}

static int ui_test_clamp_y(int y)
{
    int32_t h = display_get_height();
    if (y < 0) {
        y = 0;
    }
    if (h > 0 && y >= h) {
        y = (int)h - 1;
    }
    return y;
}

/* ========================================================================
 * SCRIPT TIMER (LVGL task)
 * ======================================================================== */

static void ui_test_script_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    uint32_t elapsed;

    if (!s_ui.running) {
        return;
    }
    elapsed = lv_tick_elaps(s_ui.start_tick);

    while (s_ui.step_idx < s_ui.step_count &&
           s_ui.steps[s_ui.step_idx].at_ms <= elapsed) {
        const ui_step_t *step = &s_ui.steps[s_ui.step_idx];
        s_ui.cur_x = step->x;
        s_ui.cur_y = step->y;
        s_ui.cur_pressed = step->pressed;
        s_ui.step_idx++;
    }

    if (s_ui.step_idx >= s_ui.step_count &&
        elapsed >= s_ui.end_ms + UI_TEST_SETTLE_MS) {
        s_ui.cur_pressed = false;
        s_ui.running = false;
        if (s_ui.done != NULL) {
            xEventGroupSetBits(s_ui.done, UI_TEST_DONE_BIT);
        }
    }

    /* Process the synthetic pointer immediately so a short press+release is
     * never missed between the indev's own (slower) read-timer samples. */
    if (s_ui.indev != NULL) {
        lv_indev_read(s_ui.indev);
    }
}

/* ========================================================================
 * INIT
 * ======================================================================== */

esp_err_t ui_test_init(void)
{
    if (s_ui.ready) {
        return ESP_OK;
    }
    if (!lvgl_port_lock(500)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_ui.lock == NULL) {
        s_ui.lock = xSemaphoreCreateMutex();
    }
    if (s_ui.done == NULL) {
        s_ui.done = xEventGroupCreate();
    }
    if (s_ui.indev == NULL) {
        s_ui.indev = lv_indev_create();
        if (s_ui.indev != NULL) {
            lv_indev_set_type(s_ui.indev, LV_INDEV_TYPE_POINTER);
            lv_indev_set_read_cb(s_ui.indev, ui_test_read_cb);
        }
    }
    if (s_ui.script_timer == NULL) {
        s_ui.script_timer = lv_timer_create(ui_test_script_timer_cb,
                                            UI_TEST_SCRIPT_PERIOD, NULL);
    }

    if (s_ui.lock != NULL && s_ui.done != NULL && s_ui.indev != NULL &&
        s_ui.script_timer != NULL) {
        s_ui.ready = true;
        s_ui.cur_pressed = false;
        ESP_LOGI(UI_TEST_TAG, "synthetic touch indev ready");
    }

    lvgl_port_unlock();
    return s_ui.ready ? ESP_OK : ESP_FAIL;
}

bool ui_test_is_ready(void)
{
    return s_ui.ready;
}

/* ========================================================================
 * SCRIPT SUBMISSION
 * ======================================================================== */

static esp_err_t ui_test_run_script(const ui_step_t *steps, int count)
{
    uint32_t timeout_ms;
    EventBits_t bits;

    if (!s_ui.ready || steps == NULL || count <= 0 ||
        count > UI_TEST_MAX_STEPS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_ui.lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_ui.running) {
        xSemaphoreGive(s_ui.lock);
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(s_ui.steps, steps, sizeof(ui_step_t) * (size_t)count);
    s_ui.step_count = count;
    s_ui.step_idx = 0;
    s_ui.end_ms = steps[count - 1].at_ms;
    timeout_ms = s_ui.end_ms + UI_TEST_SETTLE_MS + 2000;
    s_ui.start_tick = lv_tick_get();
    s_ui.running = true;
    xEventGroupClearBits(s_ui.done, UI_TEST_DONE_BIT);
    xSemaphoreGive(s_ui.lock);

    bits = xEventGroupWaitBits(s_ui.done, UI_TEST_DONE_BIT,
                               pdTRUE, pdFALSE,
                               pdMS_TO_TICKS(timeout_ms));
    return (bits & UI_TEST_DONE_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t ui_test_tap(int x, int y, uint32_t hold_ms)
{
    ui_step_t steps[2];

    if (hold_ms < 40) {
        hold_ms = 40;
    }
    x = ui_test_clamp_x(x);
    y = ui_test_clamp_y(y);
    steps[0].x = x;
    steps[0].y = y;
    steps[0].pressed = true;
    steps[0].at_ms = 0;
    steps[1].x = x;
    steps[1].y = y;
    steps[1].pressed = false;
    steps[1].at_ms = hold_ms;
    return ui_test_run_script(steps, 2);
}

esp_err_t ui_test_long_press(int x, int y, uint32_t hold_ms)
{
    if (hold_ms < 700) {
        hold_ms = 700;
    }
    return ui_test_tap(x, y, hold_ms);
}

esp_err_t ui_test_swipe(int x1, int y1, int x2, int y2,
                        uint32_t duration_ms, int steps)
{
    ui_step_t script[UI_TEST_MAX_STEPS];
    int i;
    int n = 0;

    if (steps < 1) {
        steps = 1;
    }
    if (steps > UI_TEST_MAX_STEPS - 2) {
        steps = UI_TEST_MAX_STEPS - 2;
    }
    if (duration_ms < (uint32_t)steps * UI_TEST_SCRIPT_PERIOD) {
        duration_ms = (uint32_t)steps * UI_TEST_SCRIPT_PERIOD;
    }

    x1 = ui_test_clamp_x(x1);
    y1 = ui_test_clamp_y(y1);
    x2 = ui_test_clamp_x(x2);
    y2 = ui_test_clamp_y(y2);

    /* Press at the start. */
    script[n].x = x1;
    script[n].y = y1;
    script[n].pressed = true;
    script[n].at_ms = 0;
    n++;

    /* Paced move steps. */
    for (i = 1; i <= steps; i++) {
        script[n].x = x1 + (x2 - x1) * i / steps;
        script[n].y = y1 + (y2 - y1) * i / steps;
        script[n].pressed = true;
        script[n].at_ms = duration_ms * (uint32_t)i / (uint32_t)steps;
        n++;
    }

    /* Release at the end. */
    script[n].x = x2;
    script[n].y = y2;
    script[n].pressed = false;
    script[n].at_ms = duration_ms + 30;
    n++;

    return ui_test_run_script(script, n);
}

/* ========================================================================
 * LOW-LEVEL HOLD
 * ======================================================================== */

esp_err_t ui_test_press(int x, int y)
{
    if (!s_ui.ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_ui.lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_ui.cur_x = ui_test_clamp_x(x);
    s_ui.cur_y = ui_test_clamp_y(y);
    s_ui.cur_pressed = true;
    xSemaphoreGive(s_ui.lock);
    return ESP_OK;
}

esp_err_t ui_test_move(int x, int y)
{
    if (!s_ui.ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_ui.lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_ui.cur_x = ui_test_clamp_x(x);
    s_ui.cur_y = ui_test_clamp_y(y);
    xSemaphoreGive(s_ui.lock);
    return ESP_OK;
}

esp_err_t ui_test_release(void)
{
    if (!s_ui.ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_ui.lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_ui.cur_pressed = false;
    xSemaphoreGive(s_ui.lock);
    return ESP_OK;
}

void ui_test_cancel(void)
{
    if (s_ui.lock != NULL &&
        xSemaphoreTake(s_ui.lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        s_ui.running = false;
        s_ui.cur_pressed = false;
        if (s_ui.done != NULL) {
            xEventGroupSetBits(s_ui.done, UI_TEST_DONE_BIT);
        }
        xSemaphoreGive(s_ui.lock);
    }
}
