# Custom BT Stack — raw HCI over Hosted — Coprocessor

A **bring-your-own BT stack** example. No NimBLE, no Bluedroid — the host binds
its **own raw-HCI handler** to the ESP-Hosted HCI byte-pipe through the **custom**
path, so you can drive any BT host stack you like over the co-processor's
controller.

It brings the **controller** up on the ESP-Hosted **co-processor**, binds a custom
`rx`/`tx` pair with one call — `esp_hosted_bt_host_stack_setup()` using
`ESP_HOSTED_BT_HOST_STACK_CONFIG_CUSTOM(rx, ctx)` — then sends an **HCI Reset** and
prints the controller's **Command-Complete** received on `rx`. That round-trip
(`tx` → CP controller → `rx`) is the proof the custom two-wire path works. Wire a
real stack's transport to the same `rx`/`tx` and you have hosted BT with your own
stack. See [Porting a BT stack to
ESP-Hosted](https://github.com/espressif/esp-hosted/blob/master/docs/design/bluetooth.md#porting-a-bt-stack-to-esp-hosted).

## Supported Platforms and Transports

### Supported Coprocessors (BT controller)

| Coprocessor | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C5 | ESP32-C6 | ESP32-C61 | ESP32-H2 | ESP32-S3 | ESP32-S31 | ESP32-S2 |
| :---------- | :---: | :------: | :------: | :------: | :------: | :-------: | :------: | :------: | :-------: | :------: |
| BLE support | Yes   | Yes      | Yes      | Yes      | Yes      | Yes       | Yes      | Yes      | Yes | No (no BLE) |

### Supported Host Devices

| Host Device | ESP32-P4 | ESP32-H2 | Other MCUs |
| :---------- | :------: | :------: | :--------: |
| Support     | Yes      | Yes      | [Yes](https://github.com/espressif/esp-hosted/blob/master/docs/getting-started-mcu.md) |

### Supported HCI transports

| HCI over hosted bus (VHCI) | SDIO | SPI Full-Duplex | SPI Half-Duplex | UART |
| :------------------------- | :--: | :-------------: | :-------------: | :--: |
| MCU host                   | Yes  | Yes             | Yes             | Yes  |

The co-processor runs the BT controller only — no host stack — and exposes HCI
over the hosted bus. Select the transport (must match the host):

```bash
cd examples/bluetooth/esp_hosted_custom/hci_demo/cp
idf.py set-target esp32c6
idf.py menuconfig
```

CP dependency config is **pre-set in `sdkconfig.defaults`** (do not remove):

```text
CONFIG_ESP_HOSTED_CP_FEAT_BT=y            # BT feature on the co-processor
CONFIG_ESP_HOSTED_CP_BT_ENABLED=y
CONFIG_ESP_HOSTED_CP_FEAT_BT_HCI_VHCI=y   # HCI over the hosted bus, not a UART
CONFIG_BT_ENABLED=y
CONFIG_BT_CONTROLLER_ONLY=y               # controller only — the host owns the stack
CONFIG_ESP_HOSTED_CP_FEAT_WIFI=n          # not needed here
```

Then flash and monitor:

```bash
idf.py -p <cp_usb_serial_port> flash monitor
```

<!-- generated from ../README.md — edit that file, not this one -->
