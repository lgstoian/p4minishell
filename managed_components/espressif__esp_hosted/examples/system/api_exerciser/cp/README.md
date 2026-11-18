# API exerciser — Coprocessor

A generic host+CP example that exposes the native `eh_host_*` API surface as
console commands, so functional API coverage is added as **data** (a console
command + an expected line) rather than a new example per scenario.

## Supported Platforms and Transports

### Supported Coprocessors

| Coprocessor | ESP32 | ESP32-C Series | ESP32-S2/S3 | ESP32-S31 |
| :----------: | :---: | :------------: | :---------: | :-------: |
| Support     | Yes   | Yes            | Yes         | Yes ¹     |

¹ ESP32-S31: preview target, 2.4 GHz only, no SDIO. See [ESP32-S31 test board](https://github.com/espressif/esp-hosted/blob/master/docs/getting-started-mcu.md#esp32-s31-test-board-on-the-esp32-p4-function-ev-board).

### Supported Host Devices

| Host Device | ESP32-P4 | ESP32-H2 | Other MCUs | Linux |
| :---------: | :------: | :------: | :--------: | :---: |
| Support     | Yes | Yes | [Yes](https://github.com/espressif/esp-hosted/blob/master/docs/getting-started-mcu.md) | [Yes](https://github.com/espressif/esp-hosted/blob/master/docs/getting-started-linux.md) |

### Supported Connection buses

| Connection bus | SDIO | SPI Full-Duplex | SPI Half-Duplex | UART |
| :------------- | :--: | :-------------: | :-------------: | :--: |
| Linux host     | Yes  | Yes             | No              | No   |
| MCU host       | Yes  | Yes             | Yes             | Yes  |

## Configuration

The exerciser needs every feature whose API it offers as a command, so the
co-processor config is **pre-set in `sdkconfig.defaults`** (do not remove):

```text
CONFIG_ESP_HOSTED_CP_FEAT_WIFI=y          # Wi-Fi API surface
CONFIG_ESP_HOSTED_CP_FEAT_GPIO_EXP=y      # GPIO expander API surface
CONFIG_ESP_HOSTED_CP_FEAT_CP_EXT_COEX=y   # external-coexistence API surface
```

Dropping one of these removes the matching commands, and the tests that
drive them fail rather than skip.

<!-- generated from ../README.md — edit that file, not this one -->
