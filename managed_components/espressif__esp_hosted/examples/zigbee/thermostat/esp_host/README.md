# zigbee / thermostat — Host

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

Select the transport (must match the co-processor) and configure the spinel UART to the RCP:

```bash
cd examples/zigbee/thermostat/esp_host
idf.py set-target esp32p4
idf.py menuconfig
```

```text
Component config
└── ESP-Hosted
     └── Configure host
          └── Host transport
               └── Communication bus (match the co-processor)   ← SDIO default
Example Configuration
├── (1)      Host UART port to the RCP                          (EXAMPLE_HOST_UART_PORT)
├── (11)     Host TX pin -> RCP RX                              (EXAMPLE_HOST_UART_TX_PIN)
├── (10)     Host RX pin <- RCP TX                              (EXAMPLE_HOST_UART_RX_PIN)
└── (460800) Spinel UART baud rate                              (EXAMPLE_HOST_UART_BAUDRATE)
```

The spinel UART settings belong to the example. On ESP32-P4, `sdkconfig.defaults.esp32p4` sets the pins. On other hosts, the pins are `-1` and the build fails until you set them.

The host dependency config is **pre-set in `esp_host/sdkconfig.defaults`** — do not remove:

```text
CONFIG_ZB_ENABLED=y                          # Zigbee stack (esp-zigbee-lib)
CONFIG_SPIRAM=y                              # PSRAM integration
CONFIG_ESP_HOSTED_DFLT_TASK_FROM_SPIRAM=y
CONFIG_OPENTHREAD_RX_ON_WHEN_IDLE=n          # keep in sync with RCP coexistence
```

Erase NVRAM before the first flash if you don't want stale network state
(`idf.py -p <host_usb_serial_port> erase-flash`), then flash and monitor:

```bash
idf.py -p <host_usb_serial_port> flash monitor
```

<!-- generated from ../README.md — edit that file, not this one -->
