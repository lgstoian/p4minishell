# Classic BT Discovery — Bluedroid Host over Standard HCI UART — Coprocessor

Classic Bluetooth (BR/EDR) device **and** service discovery. The Bluedroid host
stack runs on the **host** MCU with its local controller disabled; it reaches the
Bluetooth controller on the co-processor over a **dedicated physical UART in H4
mode** (not the ESP-Hosted VHCI bridge). The host's own `uart_driver.c` is bound
to Bluedroid via `esp_bluedroid_attach_hci_driver()`. This is the Bluedroid
counterpart of `bluetooth/nimble_uart/bleprph_host_only_uart` (NimBLE / BLE).

## Supported Platforms and Transports

### Supported Coprocessors (BT controller, UART-H4 mode)

Classic BR/EDR requires an ESP32 or ESP32-S31 controller — the BLE-only chips cannot serve
the classic profile this demo discovers.

| Coprocessor | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C6 | ESP32-H2 | ESP32-S3 | ESP32-S31 |
| :---------- | :---: | :------: | :------: | :------: | :------: | :------: | :-------: |
| BR/EDR (classic) | Yes | No (BLE only) | No (BLE only) | No (BLE only) | No (BLE only) | No (BLE only) | Yes ¹ |

¹ ESP32-S31 needs ESP-IDF with commit `64018e19824` (v6.2 or later). With ESP-IDF v6.1, the ESP32-S31 controller drops host commands when Bluedroid sends them quickly.

### Supported Host Devices

| Host device | ESP32-P4 | ESP32-H2 | Other MCUs |
| :---------- | :------: | :------: | :--------: |
| Support     | Yes      | Yes      | [Yes](https://github.com/espressif/esp-hosted/blob/master/docs/getting-started-mcu.md) |

### HCI transport

| HCI link           | Dedicated UART (H4) |
| :----------------- | :-----------------: |
| Host ↔ controller  | Yes — RTS/CTS flow control recommended; both ends must match |

Bluetooth HCI here runs over its own UART, independent of the ESP-Hosted bus a
host may separately use for Wi-Fi.

The co-processor runs the BT controller with HCI exposed on a physical UART (H4),
not on the ESP-Hosted bus. This example discovers **classic BR/EDR** devices.
Build the co-processor on an **ESP32** or **ESP32-S31**. These are the only
Espressif controllers with the classic profile. Keep BR/EDR enabled in the
controller:

```bash
cd examples/bluetooth/bluedroid_uart/bluedroid_host_only_uart/cp
idf.py set-target esp32              # or: idf.py --preview set-target esp32s31
idf.py menuconfig
```

ESP32 co-processor:

```text
Component config
└── Bluetooth
     └── Controller Options
          ├── Bluetooth controller mode
          │    └── (X) BR/EDR + BLE           <── classic profile must be present
          └── HCI mode
               ├── ( ) VHCI
               └── (X) UART                    <── HCI over a dedicated UART (H4)
                    ├── HCI UART Tx pin        ← wire to host UART Rx
                    ├── HCI UART Rx pin        ← wire to host UART Tx
                    └── [*] HCI UART flow control (RTS/CTS)   <── default on
```

ESP32-S31 co-processor:

```text
Component config
└── Bluetooth
     └── Bluetooth controller HCI
          ├── HCI mode
          │    └── (X) UART(H4)                <── set by sdkconfig.defaults.esp32s31
          ├── HCI uart Tx gpio / Rx gpio       ← set the pins for your board
          └── [ ] HCI uart Hardware Flow ctrl  <── default off (CONFIG_BT_CTRL_HCI_UART_FLOWCTRL)
```

> [!IMPORTANT]
> The host turns flow control on by default. The ESP32-S31 controller turns it
> off by default. Make the two ends match. Either set
> `CONFIG_BT_CTRL_HCI_UART_FLOWCTRL=y` on the co-processor and set its RTS and
> CTS pins, or clear **UART hardware flow control (RTS/CTS)** on the host.

The CP dependency config is pre-set in `sdkconfig.defaults` (do not remove):

```text
CONFIG_ESP_HOSTED_CP_FEAT_BT=y            # ESP-Hosted BT controller feature
CONFIG_ESP_HOSTED_CP_BT_ENABLED=y         # required by ESP_HOSTED_CP_FEAT_BT
CONFIG_ESP_HOSTED_CP_FEAT_BT_HCI_UART=y   # BT HCI on a dedicated UART (H4), NOT the hosted bus
CONFIG_BT_ENABLED=y                       # BT controller stack on the CP
CONFIG_BT_CONTROLLER_ONLY=y               # controller-only profile
CONFIG_ESP_HOSTED_CP_FEAT_WIFI=n          # Wi-Fi off — BT controller-only CP
```

Then flash and monitor:

```bash
idf.py -p <cp_usb_serial_port> flash monitor
```

<!-- generated from ../README.md — edit that file, not this one -->
