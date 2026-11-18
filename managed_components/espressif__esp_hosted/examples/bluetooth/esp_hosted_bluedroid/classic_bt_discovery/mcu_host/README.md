# Classic BT Discovery — Bluedroid over Hosted HCI — Host

Performs a Classic Bluetooth (BR/EDR) device inquiry and prints every device the
controller sees during the discovery window. Uses `esp_bt_gap_*` (Classic)
rather than `esp_ble_gap_*`; the bridge layer is identical to the BLE examples —
Classic vs BLE is purely a host-stack concern. Bluedroid runs on the **host**;
the BR/EDR **controller** runs on the ESP-Hosted **co-processor**, reached over
the hosted transport (SDIO / SPI / SPI-HD / UART) via VHCI.

**Classic Bluetooth (BR/EDR) needs an ESP32 or ESP32-S31 co-processor.** The
other chips are BLE-only. The host build stops with an error if the selected
co-processor has no Classic BT.

## Supported Platforms and Transports

### Supported Coprocessors (BR/EDR controller)

| Coprocessor       | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C5 | ESP32-C6 | ESP32-C61 | ESP32-H2 | ESP32-S3 | ESP32-S31 | ESP32-S2 |
| :---------------- | :---: | :------: | :------: | :------: | :------: | :-------: | :------: | :------: | :-------: | :------: |
| Classic BT (BR/EDR) | Yes | No       | No       | No       | No       | No        | No       | No       | Yes | No       |

### Supported Host Devices

| Host Device | ESP32-P4 | ESP32-H2 | Other MCUs |
| :---------- | :------: | :------: | :--------: |
| Support     | Yes      | Yes      | [Yes](https://github.com/espressif/esp-hosted/blob/master/docs/getting-started-mcu.md) |

### Supported HCI transports

| HCI over hosted bus (VHCI) | SDIO | SPI Full-Duplex | SPI Half-Duplex | UART |
| :------------------------- | :--: | :-------------: | :-------------: | :--: |
| MCU host                   | Yes  | Yes             | Yes             | Yes  |

Select the co-processor and the transport (both must match the co-processor):

```bash
cd examples/bluetooth/esp_hosted_bluedroid/classic_bt_discovery/mcu_host
idf.py set-target esp32p4
idf.py menuconfig
```

```text
Component config
└── Wi-Fi Remote
     └── choose slave target
          ├── (X) esp32c6                      <── default on ESP32-P4: no Classic BT, the build fails
          ├── ( ) esp32                        ← Classic BT
          └── ( ) esp32s31                     ← Classic BT
```

The ESP-Hosted co-processor target follows this choice. If Wi-Fi Remote is
disabled, select the co-processor in **ESP-Hosted → Configure host → Paired
coprocessor** instead.

```text
Component config
└── ESP-Hosted
     └── Configure host
          └── Host transport
               ├── Communication bus (co-processor <== bus ==> host)
               │    ├── ( ) SPI Full Duplex
               │    ├── (X) SDIO                    <── default (match the co-processor)
               │    ├── ( ) SPI Half Duplex
               │    └── ( ) UART
               └── SDIO Configuration               ← slot, bus width, GPIOs (defaults OK)
```

This example adds its own options under **Classic BT Discovery config**:

```text
Classic BT Discovery config
├── (10) Inquiry duration (×1.28 s units)                    ← range 1–48
└── [*] Log devices that don't advertise a friendly name
```

The host dependency config is pre-set in `sdkconfig.defaults` (do not remove).
`CONFIG_ESP_HOSTED_HOST_FEAT_BT` enables the BT host feature; the host stack is
chosen by the IDF BT Kconfig (`CONFIG_BT_BLUEDROID_ENABLED`) — no separate hosted
BT-port switch:

```text
CONFIG_ESP_HOSTED_HOST_FEAT_BT=y                      # host BT feature (controller runs on the CP)
CONFIG_BT_ENABLED=y                                  # BT host stack on
CONFIG_BT_CONTROLLER_DISABLED=y                      # no local controller — CP supplies it
CONFIG_BT_BLUEDROID_ENABLED=y                        # Bluedroid host stack (selects the hosted BT adapter)
CONFIG_BT_CLASSIC_ENABLED=y                          # Classic BT (BR/EDR) inquiry
CONFIG_BT_BLE_ENABLED=y                              # BLE also enabled (dual-mode)
```

`app_main()` calls `esp_hosted_bt_host_stack_setup()` (from `esp_hosted_bt_host_stack.h`)
once, after `esp_hosted_connect_to_slave()` and before `esp_bluedroid_init()` —
it brings the controller up and binds Bluedroid to the hosted HCI. The adapter
is documented in
[Porting a BT stack to ESP-Hosted](https://github.com/espressif/esp-hosted/blob/master/docs/design/bluetooth.md#porting-a-bt-stack-to-esp-hosted).

Then flash and monitor:

```bash
idf.py -p <host_usb_serial_port> flash monitor
```

### Verify

- Watch the monitor log for discovered devices during the inquiry window
  (default 10 × 1.28 s ≈ 12.8 s).
- The host build fails with `classic_bt_discovery needs an ESP32 or ESP32-S31
  co-processor` if the selected CP has no Classic BT. Select esp32 or esp32s31
  in **Wi-Fi Remote → choose slave target**.
- The host enables `CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY=y` to drive Bluedroid's
  BR/EDR feature gates even though the local controller is disabled (the CP
  supplies the controller).

<!-- generated from ../README.md — edit that file, not this one -->
