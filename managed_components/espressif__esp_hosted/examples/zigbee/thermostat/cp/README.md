# zigbee / thermostat — Coprocessor

Runs a Zigbee **Home Automation Thermostat** (a Zigbee Coordinator) on the host. The co-processor is the 802.15.4 **Radio Co-Processor (RCP)** — a standard OpenThread RCP, as in ESP-IDF `ot_rcp`, with 802.15.4 (spinel) traffic on a **dedicated UART**. ESP-Hosted does not carry spinel and does not control the RCP: the co-processor app starts it at boot. Zigbee vs OpenThread is purely a host-side choice (the host links `esp-zigbee-lib` instead of `openthread`). Adapted from the [ESP Zigbee SDK Thermostat example](https://github.com/espressif/esp-zigbee-sdk/tree/main/examples/home_automation_devices/thermostat). See the [OpenThread and Zigbee Support](https://github.com/espressif/esp-hosted-mcu/blob/main/docs/features/openthread.md) doc for the RCP architecture.

The co-processor plays two roles: the **ESP-Hosted CP** (over the bus) and
the **802.15.4 RCP** (radio, over the dedicated UART). Both are the same
ESP32-C6 (Wi-Fi off) — two roles, not two chips, and they do not touch each
other. A working deployment is 3 SoCs: host, that co-processor, and a
separate Zigbee end-device.

```text
  Host (ESP32-P4) - Zigbee Coordinator
    |
    |  ESP-Hosted bus (SDIO / SPI / UART)
    +--- ESP-Hosted link ------------> [ ESP-Hosted CP ]      (no 802.15.4 here)
    |
    |  dedicated UART
    +--- 802.15.4 spinel (data) -----> [ 802.15.4 RCP ] --- 802.15.4 RF ---> Zigbee end-device
```

Data path: host -> spinel UART -> RCP radio -> RF -> end-device (a sensor).

## Supported Platforms and Transports

### Supported Coprocessors

Zigbee uses an 802.15.4 radio co-processor (RCP) — standard OpenThread RCP firmware.

| Coprocessor | ESP32-C6 | ESP32-H2 | ESP32-H4 | ESP32-C5 |
| :---------: | :------: | :------: | :------: | :------: |
| Support     | Yes      | Yes      | Yes      | Yes      |

### Supported Host Devices

The Zigbee stack (`esp-zigbee-lib`) is ESP-IDF-only, so the host is an ESP-IDF device (role `esp_host`). PSRAM is recommended to be used.

| Host Device | ESP32-P4 | Other MCUs | Linux |
| :---------: | :------: | :--------: | :---: |
| Support     | Yes      | No (esp-zigbee-lib is ESP-IDF-only) | No |

### Supported Connection buses

The 802.15.4 spinel traffic always rides a separate dedicated UART; the ESP-Hosted bus does not carry it.

| Connection bus | SDIO | SPI Full-Duplex | SPI Half-Duplex | UART |
| :------------- | :--: | :-------------: | :-------------: | :--: |
| MCU host       | Yes  | Yes             | Yes             | Yes  |

The co-processor runs ESP-Hosted (Wi-Fi off) and a standard 802.15.4 **RCP** on the dedicated UART: the Zigbee host supports only that link. The app starts the RCP at boot, as ESP-IDF `ot_rcp` does. Select the ESP-Hosted transport:

```bash
cd examples/zigbee/thermostat/cp
idf.py set-target esp32c6
idf.py menuconfig
```

```text
Component config
└── ESP-Hosted
     └── Configure coprocessor
          └── CP transport
               └── Communication bus (co-processor <== bus ==> host)
                    ├── ( ) SPI Full Duplex
                    ├── (X) SDIO                    <── default
                    ├── ( ) SPI Half Duplex         ← MCU host only
                    └── ( ) UART                    ← MCU host only
```

The spinel UART to the host is an example setting, under *Example Configuration → Dedicated spinel UART*: `EXAMPLE_RCP_UART_PORT`, `EXAMPLE_RCP_UART_TX_PIN`, `EXAMPLE_RCP_UART_RX_PIN` and `EXAMPLE_RCP_UART_BAUDRATE`. On ESP32-C6, `sdkconfig.defaults.esp32c6` sets the pins. The port must not be the ESP-Hosted UART transport port; the build checks it.

The RCP dependency config is **pre-set in `cp/sdkconfig.defaults`** (do not remove):

```text
CONFIG_OPENTHREAD_RCP_UART=y             # standard RCP on the dedicated spinel UART
CONFIG_OPENTHREAD_ENABLED=y              # RCP is built via IDF's openthread component
CONFIG_OPENTHREAD_RADIO=y                # Radio-Only (RCP) device
CONFIG_ESP_HOSTED_CP_FEAT_WIFI=n         # Wi-Fi off on the RCP
CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y      # SW coexistence ON — matches host RX_ON_WHEN_IDLE=n
```

> [!IMPORTANT]
> `CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y` is the canonical RCP setting
> (doc §2.1). It changes the 802.15.4 capability set the RCP advertises,
> so the host **must** pair it with `CONFIG_OPENTHREAD_RX_ON_WHEN_IDLE=n`
> — a mismatch makes RCP/stack init fail.

Then flash and monitor:

```bash
idf.py -p <cp_usb_serial_port> flash monitor
```

<!-- generated from ../README.md — edit that file, not this one -->
