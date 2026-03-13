# ESP32-C6 ESP-Hosted Slave

This project builds the ESP-Hosted co-processor firmware that matches the host-side `espressif/esp_hosted ~2.12.1` dependency already locked in this repository.

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
idf.py merge-bin -o build/esp32c6_hosted_slave_merged.bin
```

The host-side `c6update` command expects the merged flash image at offset `0x0`, not the app-only `build/esp32c6_hosted_slave.bin`. Use `build/esp32c6_hosted_slave_merged.bin` for SD-card recovery updates.

## Flash

Flash the ESP32-C6 on its own serial port:

```powershell
idf.py -p <COPROCESSOR_PORT> flash monitor
```

After flashing, the host log should report a co-processor hosted version in the `2.12.x` line instead of `2.1.0`, which removes the transport version mismatch warning without downgrading host functionality.