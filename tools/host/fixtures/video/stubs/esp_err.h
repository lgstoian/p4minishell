/* Host-only ESP-IDF type shim for the video gate (test fixture, not firmware). */
#ifndef VIDEO_GATE_ESP_ERR_H
#define VIDEO_GATE_ESP_ERR_H
typedef int esp_err_t;
#define ESP_OK             0
#define ESP_FAIL          -1
#define ESP_ERR_NO_MEM    -2
#define ESP_ERR_INVALID_ARG -3
#define ESP_ERR_INVALID_STATE -4
#define ESP_ERR_NOT_FOUND -5
#define ESP_ERR_NOT_SUPPORTED -6
#endif
