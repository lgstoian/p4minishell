/* Host mock: vTaskDelay and pdMS_TO_TICKS are no-ops. */
#ifndef POWER_GATE_TASK_H
#define POWER_GATE_TASK_H
#include <stdint.h>
#define pdMS_TO_TICKS(ms) ((uint32_t)(ms))
void vTaskDelay(uint32_t ticks);
#endif
