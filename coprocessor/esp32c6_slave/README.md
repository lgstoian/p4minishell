# ESP32-C6 ESP-Hosted Slave

This project builds the ESP-Hosted co-processor firmware for the ESP32-C6 that
pairs with the P4MiniShell host firmware. It tracks the same ESP-Hosted release
line as the host dependency: the slave allows any compatible `^3.0.6` (see
`main/idf_component.yml`) while the host pins the exact `3.0.6` (see
`main/idf_component.yml`) — the two must stay on the same release line (the
must-match-host rule; the host enforces a version gate at boot).

**Slave source:** `main/main.c` (NVS + event loop; the hosted stack comes from
the managed `espressif/esp_hosted` component). `main/` also holds
`CMakeLists.txt` and `idf_component.yml`; `sdkconfig.defaults` and
`partitions.esp32c6.csv` live at the slave project root (not under `main/`).
To change slave behaviour, change the `esp_hosted` version in
`main/idf_component.yml` or add Kconfig options.

## Target

- Co-processor: `esp32c6`
- Transport: SDIO
- Board defaults: `ESP32-P4-Function-EV-Board` compatible SDIO pin map
  (`sdkconfig.defaults`); v1.3.0 also supports the M5Stack Tab5 C6 wiring
  (slot 1, CLK 12 / CMD 13 / D0-D3 11,10,9,8, reset 15, 10 MHz — see
  `boards/m5stack_tab5/board_config.h` and `boards/m5stack_tab5/sdkconfig.defaults`)
- Features: Wi-Fi + BLE (controller-only), RPC, system

## Build

The board profile is selected with `-DC6_BOARD=<name>`, which layers the shared
`sdkconfig.defaults` under `boards/<name>/sdkconfig.defaults` (the board
`choice` lives in the per-board file, not the shared one). Use a per-board build
directory so the two images never collide:

```powershell
# Reference board (JC1060P470C / ESP32-P4-Function-EV-Board SDIO map)
idf.py -B build-jc1060p470c -DC6_BOARD=jc1060p470c set-target esp32c6
idf.py -B build-jc1060p470c -DC6_BOARD=jc1060p470c build

# M5Stack Tab5
idf.py -B build-m5stack_tab5 -DC6_BOARD=m5stack_tab5 set-target esp32c6
idf.py -B build-m5stack_tab5 -DC6_BOARD=m5stack_tab5 build
```

The host-side `c6ota` command expects an **application image** (not a merged
flash image): `build-<board>/esp32c6_hosted_slave.bin`. The release stages these
per-board images as `esp32c6_hosted_slave-<board>.bin`; `apps/push_c6.py`
pushes the matching one to the SD root as `esp32c6_hosted_slave.bin`, which
`c6ota default` then flashes.

## Flash

Flash the ESP32-C6 on its own serial port:

```powershell
idf.py -B build-<board> -DC6_BOARD=<board> -p <COPROCESSOR_PORT> flash monitor
```

After flashing, the host log should report a co-processor version in the same
`3.0.x` release line the host was built against. The host enforces a version
compatibility gate at boot, so keep the two in sync (see the Wi-Fi rules in
[`../../ai-context.md`](../../ai-context.md)).

Alternatively, update the C6 from the shell over SDIO with
`c6ota sd:/path/to/app.bin` or `c6ota http(s)://...` (see
[`../../command.md`](../../command.md)).
