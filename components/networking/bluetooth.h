#ifndef P4MINISHELL_BLUETOOTH_H
#define P4MINISHELL_BLUETOOTH_H

#include <stdbool.h>

#include "networking.h"

void bluetooth_init(const networking_host_ops_t *ops);

// AI: Bluetooth enabled on C6 co-processor (nimble HCI over hosted SDIO) as per cheops/JC1060P470C_I_W + esp-hosted docs
void bluetooth_handle_command(char *command);
void bluetooth_status(void);
void bluetooth_scan(void);
void bluetooth_advertise(bool enable);
bool bluetooth_is_enabled(void);
bool bluetooth_is_connected(void);

#endif