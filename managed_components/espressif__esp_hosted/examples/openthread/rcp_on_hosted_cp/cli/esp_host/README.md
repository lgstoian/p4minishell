# OpenThread CLI — RCP on the ESP-Hosted co-processor — Host

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

```bash
cd examples/openthread/rcp_on_hosted_cp/cli/esp_host
idf.py set-target esp32p4
idf.py menuconfig
```

Check two settings. Both must match the co-processor:

1. **Bus to the co-processor:**

   ```text
   Component config → ESP-Hosted → Configure host → Host transport
     └── Communication bus:  (X) SDIO   ( ) SPI Full Duplex   ( ) SPI Half Duplex   ( ) UART
   ```

2. **Spinel link** (ESP-IDF OpenThread menu):

   ```text
   Component config → OpenThread → Thread Core Features → Thread 15.4 Radio Link
     └── Config the Thread radio type with 15.4 link:  (X) Connect via Custom Transport   ( ) Connect via UART
   ```

   - **Connect via Custom Transport** (default): hosted OpenThread, with `Features → OpenThread RCP on the co-processor` on.
   - **Connect via UART**: standard OpenThread. ESP-Hosted has no part in it. Set the UART in *Example Configuration → Dedicated spinel UART* (`EXAMPLE_HOST_UART_PORT`, `_TX_PIN`, `_RX_PIN`, `_BAUDRATE`). `sdkconfig.defaults.esp32p4` sets the P4 pins.

These settings are already in `esp_host/sdkconfig.defaults` (and `sdkconfig.defaults.esp32p4`). Keep them:

```text
CONFIG_ESP_HOSTED_HOST_FEAT_OPENTHREAD=y     # hosted OpenThread feature
CONFIG_ESP_HOSTED_HOST_FEAT_RPC_EXT_V2=y     # needed by the feature
CONFIG_OPENTHREAD_ENABLED=y
CONFIG_OPENTHREAD_RADIO_SPINEL_CUSTOM=y      # spinel on the ESP-Hosted bus
CONFIG_SPIRAM=y                              # the host does not start without PSRAM
CONFIG_OPENTHREAD_RX_ON_WHEN_IDLE=n          # pairs with the co-processor's SW coexistence
```

Flash and monitor:

```bash
idf.py -p <host_usb_serial_port> flash monitor
```

<!-- generated from ../README.md — edit that file, not this one -->
