# ESP32-C6 ESP-Hosted Slave

This project builds the ESP-Hosted co-processor firmware that matches the host-side `espressif/esp_hosted 2.12.1` dependency restored in this repository.

It reuses the checked-in upstream slave sources from `managed_components/espressif__esp_hosted/slave`, so the host and co-processor stay on the same ESP-Hosted release line.

## Target

- Co-processor: `esp32c6`
- Transport: `SDIO`
- Board defaults: `ESP32-P4-Function-EV-Board` compatible SDIO pin map

## Build

From this directory:

```powershell
idf.py set-target esp32c6
idf.py build
```

The host-side `c6ota` command expects an ESP-IDF application image, not a merged flash image. Use the app artifact in `build/esp32c6_hosted_slave.bin` for hosted OTA updates from SD or HTTP.

## Flash

Flash the ESP32-C6 on its own serial port:

```powershell
idf.py -p <COPROCESSOR_PORT> flash monitor
```

After flashing, the host log should report a co-processor hosted version in the `2.12.x` release line, which keeps the SDIO transport aligned with the host build used by `c6ota` and normal boot-time Wi-Fi.