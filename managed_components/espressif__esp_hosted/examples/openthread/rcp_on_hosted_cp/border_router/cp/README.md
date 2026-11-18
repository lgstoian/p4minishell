# Thread Border Router — RCP on the ESP-Hosted co-processor — Coprocessor

A Thread Border Router on the host. It connects a Thread mesh to your Wi-Fi network. One co-processor gives the host **two radios**: the 802.15.4 radio for Thread (the Radio Co-Processor, RCP) and Wi-Fi for the backhaul. The host runs the full border router stack (border routing, mDNS, NAT64). Adapted from `esp-idf/examples/openthread/ot_br`.

## How it works

The host runs the OpenThread Border Router (mDNS, NAT64). The co-processor is its 802.15.4 radio, the Radio Co-Processor (RCP). Spinel, the protocol between the Thread stack and the radio, takes one of two paths. Pick one; both sides must match.

### Default: spinel over the ESP-Hosted bus (hosted OpenThread)

Nothing to set: `sdkconfig.defaults` select this. No extra wires.

```mermaid
flowchart LR
    subgraph H["Host · ESP32-P4"]
        OT["OpenThread Border Router"]
    end
    subgraph C["Co-processor · ESP32-C6 / C5"]
        F["ESP-Hosted OpenThread feature"] --> RCP["802.15.4 radio (RCP)"]
        WIFI["Wi-Fi"]
    end
    OT <-->|"ESP-Hosted bus (SDIO / SPI / UART)<br/>RPC + spinel + Wi-Fi data"| F
    RCP <-.->|"802.15.4 RF"| P["Thread mesh"]
    WIFI <-.->|"Wi-Fi RF"| LAN["Wi-Fi AP / LAN"]
```

Needs an ESP-IDF with the OpenThread custom transport (ESP-IDF master).

### Option: spinel on a dedicated UART (standard OpenThread)

Like ESP-IDF `ot_rcp` + `ot_br`: ESP-Hosted does not carry spinel. Two extra wires (TX, RX). Use it when your ESP-IDF has no custom transport; ESP-IDF then selects UART on its own.

```mermaid
flowchart LR
    subgraph H["Host · ESP32-P4"]
        OT["OpenThread Border Router"]
    end
    subgraph C["Co-processor · ESP32-C6 / C5"]
        A["app_main() starts the RCP"] --> RCP["802.15.4 radio (RCP)"]
        WIFI["Wi-Fi"]
    end
    OT <-->|"dedicated UART: spinel"| A
    H <-.->|"ESP-Hosted bus: RPC + Wi-Fi data"| C
    RCP <-.->|"802.15.4 RF"| P["Thread mesh"]
    WIFI <-.->|"Wi-Fi RF"| LAN["Wi-Fi AP / LAN"]
```

| Set this | Co-processor (`cp`) | Host (`esp_host`) |
| :--- | :--- | :--- |
| Spinel link (ESP-IDF OpenThread menu) | `CONFIG_OPENTHREAD_RCP_UART=y` | `CONFIG_OPENTHREAD_RADIO_SPINEL_UART=y` |
| UART pins, port, baud (Example Configuration) | `EXAMPLE_RCP_UART_*` | `EXAMPLE_HOST_UART_*` |

The ESP-Hosted OpenThread feature then stays inactive on both sides; leave its setting as it is.

### Start-up (default path)

```mermaid
sequenceDiagram
    participant App as Host app (IDF ot_br code)
    participant EH as ESP-Hosted (host)
    participant CP as Co-processor
    EH->>CP: at boot: connect over the ESP-Hosted bus
    CP-->>EH: capabilities (Wi-Fi, "OpenThread over ESP-Hosted")
    App->>EH: esp_openthread_start(): first spinel write
    EH->>EH: open the spinel link
    EH->>CP: RPC: start the 802.15.4 radio
    EH->>CP: spinel: reset and configure the radio
    App->>CP: RPC: connect Wi-Fi (ot wifi connect)
    Note over App,CP: Thread mesh and Wi-Fi LAN are bridged
```

The app is the ESP-IDF `ot_br` code. In `esp_ot_config.h`, `ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG()` is `ESP_HOSTED_OPENTHREAD_RADIO_CONFIG()` on the ESP-Hosted transport. On the dedicated UART, it is a UART config from `EXAMPLE_HOST_UART_*`, and ESP-Hosted has no part in OpenThread.

> [!NOTE]
> Thread and Wi-Fi share one radio on the ESP32-C6, so each gets part of the air time. For two separate radios (P4 + C6 for Wi-Fi + H2 for Thread), see the [ESP-Thread-BR P4 example](https://github.com/espressif/esp-thread-br/blob/main/examples/basic_thread_border_router/README_esp32p4.md) and the [C6 coexistence guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-guides/coexist.html).

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

The co-processor is the 802.15.4 radio and the Wi-Fi backhaul.

```bash
cd examples/openthread/rcp_on_hosted_cp/border_router/cp
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
CONFIG_ESP_HOSTED_CP_FEAT_WIFI=y         # Wi-Fi backhaul
CONFIG_OPENTHREAD_RADIO=y                # radio-only device (RCP)
CONFIG_OPENTHREAD_RCP_CUSTOM=y           # spinel on the ESP-Hosted bus
CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y      # Wi-Fi and 802.15.4 share one radio
```

Flash and monitor:

```bash
idf.py -p <cp_usb_serial_port> flash monitor
```

<!-- generated from ../README.md — edit that file, not this one -->
