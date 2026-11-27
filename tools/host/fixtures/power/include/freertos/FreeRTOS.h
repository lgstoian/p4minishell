/* Host mock for FreeRTOS primitives used by power_commands.c. */
#ifndef POWER_GATE_FREERTOS_H
#define POWER_GATE_FREERTOS_H
#include <stdint.h>
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux)  ((void)(mux))
#endif
