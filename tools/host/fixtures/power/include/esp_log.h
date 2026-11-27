/* Host mock: logging is a no-op in the power gate harness. */
#ifndef POWER_GATE_ESP_LOG_H
#define POWER_GATE_ESP_LOG_H
#define ESP_LOGI(tag, ...) ((void)0)
#define ESP_LOGW(tag, ...) ((void)0)
#define ESP_LOGE(tag, ...) ((void)0)
#endif
