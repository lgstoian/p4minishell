# License

P4MiniShell is open-source software released under the **MIT License**.

```
MIT License

Copyright (c) 2026 Stoian Alexandru

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

The full text also ships as [`LICENSE`](LICENSE) at the repository root, and
every project-authored source file carries an `SPDX-License-Identifier: MIT`
header.

## What the MIT license covers

The MIT license above applies to the original P4MiniShell code: the shell,
components, batch engine, command verbs, editor, TUI/GFX layers, host tools,
unit tests, board metadata, and this documentation. You are free to use,
modify, and redistribute them, including commercially, as long as the
copyright notice and permission notice are preserved.

## Third-party components

P4MiniShell builds on third-party open-source components that keep their own
licenses. The MIT license above does **not** replace those terms. The table
below lists the components that ship in this repository and their licenses.

### Vendored components (`managed_components/`)

| Component | License |
|-----------|---------|
| `espressif__esp_hosted` | Apache License 2.0 |
| `espressif__esp_wifi_remote` | Apache License 2.0 |
| `espressif__wifi_remote_over_eppp` | Apache License 2.0 |
| `espressif__eppp_link` | Apache License 2.0 |
| `espressif__esp_lvgl_port` | Apache License 2.0 |
| `espressif__esp32_p4_function_ev_board` | Apache License 2.0 |
| `espressif__esp_codec_dev` | Apache License 2.0 |
| `espressif__esp_serial_slave_link` | Apache License 2.0 |
| `espressif__led_strip` | Apache License 2.0 |
| `espressif__usb` | Apache License 2.0 |
| `espressif__usb_host_msc` | Apache License 2.0 |
| `espressif__usb_host_hid` | Apache License 2.0 |
| `espressif__usb_host_cdc_acm` | Apache License 2.0 |
| `espressif__esp_lcd_jd9165` | Apache License 2.0 |
| `espressif__esp_lcd_ili9881c` | Apache License 2.0 |
| `espressif__esp_lcd_ek79007` | Apache License 2.0 |
| `espressif__esp_lcd_lt8912b` | Apache License 2.0 |
| `espressif__esp_lcd_touch` | Apache License 2.0 |
| `espressif__esp_lcd_touch_gt911` | Apache License 2.0 |
| `espressif__cmake_utilities` | Apache License 2.0 |
| `lvgl__lvgl` | MIT License |
| `espressif__esp_hosted/common/protobuf-c` | BSD-style license (protobuf-c project) |

The exact license texts ship inside each component directory
(`LICENSE`, `LICENCE`, `license.txt`, or equivalent). Some Espressif
components bundle additional third-party sources under their own notices; those
notices are preserved verbatim inside the component.

### Board support packages vendored in-tree (`boards/*/board_bsp/`)

Each board profile stages its BSP as the `board_bsp` component. Two profiles
exist, and one of them vendors upstream sources directly:

| Path | Origin | Copyright | License |
|------|--------|-----------|---------|
| `boards/m5stack_tab5/board_bsp/` | Sources trimmed from the Espressif **`m5stack_tab5`** BSP (`espressif/m5stack_tab5`, part of [espressif/esp-bsp](https://github.com/espressif/esp-bsp) and mirrored by M5Stack's [M5Tab5-UserDemo](https://github.com/m5stack/M5Tab5-UserDemo)) | © 2025-2026 Espressif Systems (Shanghai) CO LTD | Apache License 2.0 (see `boards/m5stack_tab5/board_bsp/LICENSE`) |
| `boards/jc1060p470c/board_bsp/` | Project-authored thin wrapper over the managed `espressif__esp32_p4_function_ev_board` component | © 2026 Stoian Alexandru | MIT License |

The vendored Tab5 BSP keeps the upstream `SPDX-FileCopyrightText: Espressif
Systems (Shanghai) CO LTD` and `SPDX-License-Identifier: Apache-2.0` headers on
every file taken from the upstream BSP. This repository only removes sources for
features the port does not use (the camera and IMU/sensor-hub blocks); it does
not relicense them. Project-authored glue (`board_bsp.h`,
`extra_dirs.cmake`, the CMake wiring, and `board_config.h`) is MIT.

The M5Stack [M5Tab5-UserDemo](https://github.com/m5stack/M5Tab5-UserDemo) and
[M5Tab5-Keyboard-UserDemo](https://github.com/m5stack/M5Tab5-Keyboard-UserDemo)
(MIT) were used as the reference for the Tab5 hardware bring-up and the
on-board device protocols. Most protocol knowledge (not source) was re-expressed
in this project's own drivers; the BMI270 wrapper is vendored verbatim:

| This project | Adapted from | Terms |
|--------------|--------------|-------|
| `components/tab5kbd/` | M5Tab5-Keyboard-UserDemo `m5_tab5_keyboard.*` (register map, HID/RGB protocol) | MIT, © 2025-2026 M5Stack |
| `components/power_monitor/` | M5Tab5-UserDemo `power_monitor_ina226` (INA226 config/calibration) and the TI INA226 datasheet (SBOS547) | MIT (driver) / TI datasheet |
| `components/clock/clock_rtc.c` (RX8130CE flavour) | M5Tab5-Keyboard-UserDemo `rtc_rx8130` (register map) | MIT, © M5Stack |
| `components/imu/` (`accel_gyro_bmi270.*`) | M5Stack M5Tab5-UserDemo `sensor_bmi270` (MIT); local change: no `ESP_ERROR_CHECK` on I2C add | MIT, © 2025-2026 M5Stack |
| `components/imu/` (`bmi2.*`, `bmi270*.c`, `bmi2_ois.*`) | Bosch Sensortec BMI270 Sensor API | BSD-3-Clause, © 2023 Bosch Sensortec GmbH |
| `components/camera/` | Espressif `esp_video` V4L2 usage pattern (M5Tab5-UserDemo `hal_camera.cpp`) | Apache-2.0 (Espressif) / MIT (M5Stack) |

The `espressif/esp_video`, `espressif/esp_cam_sensor`, `espressif/esp_ipa`, and
`espressif/esp_sccb_intf` camera stack are consumed as managed components and
keep their own Apache-2.0 licenses.

The co-processor firmware this project ships is built from its own
`coprocessor/esp32c6_slave` project and the `esp_hosted` component.

### Platform and toolchain

- **ESP-IDF** and the Espressif components it provides are distributed under
  their own licenses (predominantly Apache License 2.0).
- **FreeRTOS**, **lwIP**, **FatFs**, **mbedTLS**, and **protobuf-c** are
  provided through ESP-IDF and keep their upstream licenses.

### Bundled assets

| Asset | License |
|-------|---------|
| `assets/fonts/DejaVuSansMono*.ttf` | Bitstream Vera / DejaVu license (`assets/fonts/LICENSE-DejaVu.txt`) |
| `assets/fonts/DejaVuSerif*.ttf` | Bitstream Vera / DejaVu license (`assets/fonts/LICENSE-DejaVu.txt`) |
| `assets/fonts/Noto*.ttf` | SIL Open Font License 1.1 (`assets/fonts/OFL.txt`) |
| `components/gfx/gfx_font.c` | Generated from the public-domain `unscii-8` font |
| `icon/icon-512.png`, `main/assets/icon_splash.c` | Project-authored derivatives |

## Distribution rules

If you redistribute P4MiniShell or a derivative work, you must:

- keep [`LICENSE`](LICENSE) and this `licence.md`;
- keep every third-party `LICENSE`, `LICENCE`, `license.txt`, or equivalent
  notice shipped with the components you redistribute;
- preserve the upstream copyright and attribution notices those licenses
  require.

## Disclaimer

This file is a project-level summary, not legal advice. It does not rewrite the
license terms of any third-party code included in the repository. When in
doubt, consult the upstream component's own license text.
