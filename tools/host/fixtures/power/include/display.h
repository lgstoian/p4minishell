/* Host mock of display.h, including the LVGL indev probe used by idle wake. */
#ifndef POWER_GATE_DISPLAY_H
#define POWER_GATE_DISPLAY_H
#include <stddef.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
    DISPLAY_POWER_ON = 0,
    DISPLAY_POWER_SLEEP,
    DISPLAY_POWER_OFF,
} display_power_state_t;
typedef enum {
    DISPLAY_ROTATION_0 = 0,
    DISPLAY_ROTATION_90,
    DISPLAY_ROTATION_180,
    DISPLAY_ROTATION_270,
} display_rotation_t;
typedef struct lv_indev_t lv_indev_t;
struct lv_indev_t { int mock_id; };
#define LV_INDEV_STATE_PRESSED 1
lv_indev_t *lv_indev_get_next(lv_indev_t *indev);
int lv_indev_get_state(lv_indev_t *indev);
esp_err_t display_set_brightness(int percent);
esp_err_t display_rotation_parse(const char *str, display_rotation_t *out);
esp_err_t display_set_rotation(display_rotation_t rotation);
esp_err_t display_set_power_state(display_power_state_t state);
display_power_state_t display_get_power_state(void);
int display_get_touch_int_gpio(void);
#ifdef __cplusplus
}
#endif
#endif
