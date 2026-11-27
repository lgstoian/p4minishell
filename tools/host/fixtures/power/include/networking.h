/* Host mock of the Wi-Fi shutdown/restore surface used by sleep verbs. */
#ifndef POWER_GATE_NETWORKING_H
#define POWER_GATE_NETWORKING_H
#include <stdbool.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    bool should_restore_runtime;
    int mock_field;
} networking_wifi_restore_state_t;
bool networking_wifi_is_connected(void);
esp_err_t networking_wifi_shutdown(void);
void networking_wifi_capture_restore_state(networking_wifi_restore_state_t *out);
void networking_wifi_request_wake_restore(const networking_wifi_restore_state_t *state);
#ifdef __cplusplus
}
#endif
#endif
