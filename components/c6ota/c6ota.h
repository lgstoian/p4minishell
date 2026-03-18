#ifndef P4MINISHELL_C6OTA_H
#define P4MINISHELL_C6OTA_H

#include <stdbool.h>

typedef void (*c6ota_progress_callback_t)(int percent, const char *msg);

// AI: c6ota fully refactored to separate module with identical public API and 100% same behavior
// AI: API preserved for shell parser - only moved code, no functional change
// AI: SDK.md and API.md created for modular OTA usage
void c6ota_init(void);
void c6ota_perform(const char *source);
void c6ota_register_progress_callback(c6ota_progress_callback_t cb);

bool c6ota_try_handle_input(const char *input);
bool c6ota_is_busy(void);
bool c6ota_is_confirmation_pending(void);

#endif