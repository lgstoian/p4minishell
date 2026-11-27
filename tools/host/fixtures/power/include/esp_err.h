/* Host mock for ESP-IDF esp_err.h - gate fixture, not shipped firmware. */
#ifndef POWER_GATE_ESP_ERR_H
#define POWER_GATE_ESP_ERR_H
#ifdef __cplusplus
extern "C" {
#endif
typedef int esp_err_t;
#define ESP_OK              0
#define ESP_FAIL            -1
#define ESP_ERR_NOT_FOUND   0x105
const char *esp_err_to_name(esp_err_t error);
#ifdef __cplusplus
}
#endif
#endif
