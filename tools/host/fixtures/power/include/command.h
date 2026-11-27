/* Host mock of command.h (kept minimal: power_commands.c defines the verbs). */
#ifndef POWER_GATE_COMMAND_H
#define POWER_GATE_COMMAND_H
#include "esp_err.h"
void command_history_save_now(void);
#endif
