/* Host-only no-op logging shim for the video gate (test fixture, not firmware). */
#ifndef VIDEO_GATE_ESP_LOG_H
#define VIDEO_GATE_ESP_LOG_H
#define ESP_LOGW(tag, fmt, ...) ((void)0)
#define ESP_LOGE(tag, fmt, ...) ((void)0)
#define ESP_LOGI(tag, fmt, ...) ((void)0)
#endif
