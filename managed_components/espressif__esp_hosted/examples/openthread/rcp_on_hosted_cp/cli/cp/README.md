# OpenThread CLI — RCP on the ESP-Hosted co-processor — Coprocessor

Runs the OpenThread CLI (`ot ...`) on the host. The host runs the Thread stack. The co-processor is only the 802.15.4 **radio**, called the Radio Co-Processor (RCP). Adapted from `esp-idf/examples/openthread/ot_cli`.

## How it works

The host runs the OpenThread stack and its CLI (`ot ...`). The co-processor is its 802.15.4 radio, the Radio Co-Processor (RCP). Spinel, the protocol between the Thread stack and the radio, takes one of two paths. Pick one; both sides must match.

### Default: spinel over the ESP-Hosted bus (hosted OpenThread)

Nothing to set: `sdkconfig.defaults` select this. No extra wires.

```mermaid
flowchart LR
    subgraph H["Host · ESP32-P4"]
        OT["OpenThread stack + ot CLI"]
    end
    subgraph C["Co-processor · ESP32-C6 / C5"]
        F["ESP-Hosted OpenThread feature"] --> RCP["802.15.4 radio (RCP)"]
    end
    OT <-->|"ESP-Hosted bus (SDIO / SPI / UART)<br/>RPC + spinel"| F
    RCP <-.->|"802.15.4 RF"| P["Thread peer"]
```

Needs an ESP-IDF with the OpenThread custom transport (ESP-IDF master).

### Option: spinel on a dedicated UART (standard OpenThread)

Like ESP-IDF `ot_rcp` + `ot_cli`: ESP-Hosted does not carry spinel. Two extra wires (TX, RX). Use it when your ESP-IDF has no custom transport; ESP-IDF then selects UART on its own.

```mermaid
flowchart LR
    subgraph H["Host · ESP32-P4"]
        OT["OpenThread stack + ot CLI"]
    end
    subgraph C["Co-processor · ESP32-C6 / C5"]
        A["app_main() starts the RCP"] --> RCP["802.15.4 radio (RCP)"]
    end
    OT <-->|"dedicated UART: spinel"| A
    H <-.->|"ESP-Hosted bus: RPC only (no spinel)"| C
    RCP <-.->|"802.15.4 RF"| P["Thread peer"]
```

| Set this | Co-processor (`cp`) | Host (`esp_host`) |
| :--- | :--- | :--- |
| Spinel link (ESP-IDF OpenThread menu) | `CONFIG_OPENTHREAD_RCP_UART=y` | `CONFIG_OPENTHREAD_RADIO_SPINEL_UART=y` |
| UART pins, port, baud (Example Configuration) | `EXAMPLE_RCP_UART_*` | `EXAMPLE_HOST_UART_*` |

The ESP-Hosted OpenThread feature then stays inactive on both sides; leave its setting as it is.

### Start-up (default path)

```mermaid
sequenceDiagram
    participant App as Host app (IDF ot_cli code)
    participant EH as ESP-Hosted (host)
    participant CP as Co-processor (RCP)
    EH->>CP: at boot: connect over the ESP-Hosted bus
    CP-->>EH: capabilities (includes "OpenThread over ESP-Hosted")
    App->>EH: esp_openthread_start(): first spinel write
    EH->>EH: open the spinel link
    EH->>CP: RPC: start the radio
    EH->>CP: spinel: reset and configure the radio
    Note over App,CP: ot commands now work
```

The app is the ESP-IDF `ot_cli` code. Only `esp_ot_config.h` changes: on a host without an 802.15.4 radio, `ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG()` is `ESP_HOSTED_OPENTHREAD_RADIO_CONFIG()` on the ESP-Hosted transport. On the dedicated UART, it is a UART config from `EXAMPLE_HOST_UART_*`, and ESP-Hosted has no part in OpenThread.

If the two sides use different spinel links, the host stops at start-up and logs `co-processor does not run OpenThread over the ESP-Hosted transport`.

## Supported Platforms and Transports

### Supported Coprocessors

OpenThread uses an 802.15.4 radio co-processor (RCP).

| Coprocessor | ESP32-C6 | ESP32-H2 | ESP32-H4 | ESP32-C5 |
| :---------: | :------: | :------: | :------: | :------: |
| Support     | Yes      | Yes      | Yes      | Yes      |

### Supported Host Devices

| Host Device | ESP32-P4 | ESP32-H2 | Other MCUs | Linux |
| :---------: | :------: | :------: | :--------: | :---: |
| Support     | Yes | Yes | [Yes](https://github.com/espressif/esp-hosted/blob/master/docs/getting-started-mcu.md) | [Yes](https://github.com/espressif/esp-hosted/blob/master/docs/getting-started-linux.md) |

### Supported Connection buses

| Connection bus | SDIO | SPI Full-Duplex | SPI Half-Duplex | UART |
| :------------- | :--: | :-------------: | :-------------: | :--: |
| Linux host     | Yes  | Yes             | No              | No   |
| MCU host       | Yes  | Yes             | Yes             | Yes  |

The co-processor is only the radio (Wi-Fi off).

```bash
cd examples/openthread/rcp_on_hosted_cp/cli/cp
idf.py set-target esp32c6
idf.py menuconfig
```

Check two settings. Both must match the host. The defaults are correct for most setups.

1. **Bus to the host:**

   ```text
   Component config → ESP-Hosted → Configure coprocessor → CP transport
     └── Communication bus:  (X) SDIO   ( ) SPI Full Duplex   ( ) SPI Half Duplex   ( ) UART
   ```

2. **Spinel link** (ESP-IDF OpenThread menu):

   ```text
   Component config → OpenThread → Thread Core Features → Thread Radio Co-Processor Feature
     └── The RCP transport type:  (X) Custom Transport RCP   ( ) UART RCP
   ```

   - **Custom Transport RCP** (default): hosted OpenThread. Spinel goes over the ESP-Hosted bus, and `Features → OpenThread RCP` must stay on.
   - **UART RCP**: standard OpenThread. The app starts the RCP at boot on a dedicated UART, and ESP-Hosted does not carry spinel. Set the UART in *Example Configuration → Dedicated spinel UART* (`EXAMPLE_RCP_UART_PORT`, `_TX_PIN`, `_RX_PIN`, `_BAUDRATE`). `sdkconfig.defaults.esp32c6` sets the C6 pins. The port must not be the ESP-Hosted UART transport port.

These settings are already in `cp/sdkconfig.defaults`. Keep them:

```text
CONFIG_ESP_HOSTED_CP_FEAT_OPENTHREAD=y   # hosted OpenThread feature
CONFIG_OPENTHREAD_RADIO=y                # radio-only device (RCP)
CONFIG_OPENTHREAD_RCP_CUSTOM=y           # spinel on the ESP-Hosted bus
CONFIG_ESP_HOSTED_CP_FEAT_WIFI=n         # Wi-Fi off
CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y      # pairs with the host's CONFIG_OPENTHREAD_RX_ON_WHEN_IDLE=n
```

Flash and monitor:

```bash
idf.py -p <cp_usb_serial_port> flash monitor
```

<!-- generated from ../README.md — edit that file, not this one -->
