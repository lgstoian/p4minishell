/* Host mock: deterministic clock from mock_state.c. */
#ifndef POWER_GATE_ESP_TIMER_H
#define POWER_GATE_ESP_TIMER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int64_t esp_timer_get_time(void);
#ifdef __cplusplus
}
#endif
#endif
