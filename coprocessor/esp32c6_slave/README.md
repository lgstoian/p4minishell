# ESP32-C6 ESP-Hosted Slave

This project builds the ESP-Hosted co-processor firmware for the ESP32-C6 that
pairs with the P4MiniShell host firmware. It tracks the same ESP-Hosted release
line as the host dependency (`espressif/esp_hosted ^3.0.6`, see
`main/idf_component.yml`).

**No custom slave source code.** `main/` contains only build configuration
(`CMakeLists.txt`, `idf_component.yml`, `sdkconfig.defaults`,
`partitions.esp32c6.csv`) - there are no `.c` files. The slave firmware is built
entirely from the managed `espressif/esp_hosted` component's upstream slave
sources. To change slave behaviour, change the `esp_hosted` version in
`idf_component.yml` or add Kconfig options.

## Target

- Co-processor: `esp32c6`
- Transport: SDIO
- Board defaults: `ESP32-P4-Function-EV-Board` compatible SDIO pin map
- Features: Wi-Fi + BLE (controller-only), RPC, system

## Build

From this directory:

```powershell
idf.py set-target esp32c6
idf.py build
```

The host-side `c6ota` command expects an **application image** (not a merged
flash image). Use `build/esp32c6_hosted_slave.bin`.

## Flash

Flash the ESP32-C6 on its own serial port:

```powershell
idf.py -p <COPROCESSOR_PORT> flash monitor
```

After flashing, the host log should report a co-processor version in the same
`3.0.x` release line the host was built against. The host enforces a version
compatibility gate at boot, so keep the two in sync (see the Wi-Fi rules in
[`../../ai-context.md`](../../ai-context.md)).

Alternatively, update the C6 from the shell over SDIO with
`c6ota sd:/path/to/app.bin` or `c6ota http(s)://...` (see
[`../../command.md`](../../command.md)).
