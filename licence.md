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
components, batch engine, command verbs, editor, TUI/GFX layers, the built-in
retro emoji map and its LVGL adapter, host tools, unit tests, board metadata,
and this documentation. You are free to use, modify, and redistribute them,
including commercially, as long as the copyright notice and permission notice
are preserved.

### Emoji packs

The default emoji look is the project's own MIT map
(`components/font/emoji_core.c`): no third-party emoji artwork ships in the
repository. A user-installed pack under `EMOJI/` on SD or USB is the user's
responsibility, including its licence; the firmware only reads the files.
Packs that bundle Noto Emoji (OFL 1.1), Twemoji (CC-BY 4.0) or similar are
compatible with this project as long as their own notices travel with the
files.

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

`components/imgdec/` decodes PNG with **LodePNG** and JPEG with **TJpgDec** —
both ship inside the vendored `lvgl__lvgl` component (`src/libs/lodepng`,
`src/libs/tjpgd`) and keep their own notices there: LodePNG is zlib-licensed
(© Lode Vandevenne), TJpgDec is © ChaN (2021). `imgdec` is a thin wrapper and
adds no new third-party source; the licenses above already cover them.

The exact license texts ship inside each component directory
(`LICENSE`, `LICENCE`, `license.txt`, or equivalent). Some Espressif
components bundle additional third-party sources under their own notices; those
notices are preserved verbatim inside the component.

### Design influence recorded, no code copied

Two public browser projects were reviewed for ideas and deliberately **not**
copied, because neither ships a licence file (GitHub reports `license: null` for
both), so their code is "all rights reserved" regardless of any README claim:

| Project | Licence status | What was taken |
|---------|----------------|----------------|
| `alunmorris/babe32-ESP32-web-browser-for-cheap-black-display` | README claims MIT, **no LICENSE file in the repo** | Design only: a deliberate PSRAM budget (source buffer / parse pool / image buffers / history), pool-allocating the parse output instead of per-node `malloc`, and keeping the network off the UI task. |
| `mactjaap/mini_browser` | **No licence file** | Design only: an LRU image cache, a back/forward page cache that stores rendered pages, background image loading, find-in-page, and a reader view. |

Copyright protects expression, not ideas, so the architecture above is
re-expressed in our own code (`components/browser/`, the `components/tui` page
snapshots) with our own naming, structure and implementation. No file, function
or table was copied and no licence text is required, because none was
reproduced. The implementation deliberately **surpasses** their limits: we keep
QuickJS (neither has JS), a CSS cascade (babe32 has none), native PNG/JPEG
decode (they need a resize proxy), CA-store TLS (babe32 accepts any
certificate), a 1024x600 panel and 32 MB of PSRAM (versus 480x320 and 8 MB).

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
| `components/font/emoji_core.c` (retro emoji map) | Project-authored, MIT - the built-in default emoji pack. No third-party emoji artwork is bundled. |
| `apps/emoji/` (sample pack BMPs + INI) | Project-authored, MIT - a deployable example/fixture for the pack format. |
| `icon/icon-512.png`, `main/assets/icon_splash.c` | Project-authored derivatives |

### Code adapted from ESP-Brookesia (Apache-2.0)

One host tool is a **derivative work** of Espressif's
[ESP-Brookesia](https://github.com/espressif/esp-brookesia) (Apache-2.0). It
keeps the upstream copyright and license header, and the full Apache-2.0 text
ships at [`tools/LICENSE-Apache-2.0.txt`](tools/LICENSE-Apache-2.0.txt):

| This project | Adapted from | Terms |
|--------------|--------------|-------|
| `tools/font_subset.py` | `gui/brookesia_gui_interface/tools/font/charset_filter.py` | Apache-2.0, © Espressif Systems (Shanghai) CO LTD |

Local changes to that file are marked in its header: the input charset file is
optional (the GB2312/GBK candidate sets are generated from the encoding
tables), P4MiniShell-specific defaults were added, and the branding was
reworded. The upstream `charset_zh_en_gbk.txt` data file is **not** vendored.

The rest of this repository is MIT, including the files below. They were
written for P4MiniShell and are **not** derivative works, but they adopt
ESP-Brookesia's *designs* (an idea or a data format is not copyrightable
expression); the credit is recorded here rather than in the code:

| This project | Design credit |
|--------------|---------------|
| `pkg install`/`pkg unpack`/`pkg scan`, `.PKG` bundles, staged installs | ESP-Brookesia `.bpk` ZIP packages, boot-time `<volume>/apps` scan, staging + rollback install |
| `store` verb | ESP-Brookesia `brookesia_app_store` index/metadata/download flow |
| `tools/lvgl_image_pack.py` | ESP-Brookesia `cmake/image_pack.cmake` (LVGL v9 `.bin` + `imageSet` index) |
| `tools/make_pkg.py` | ESP-Brookesia `.bpk` container layout |
| `tools/dev_mcp_server.py` | ESP-Brookesia `@brookesia/app-dev-plugin` (MCP dev plugin) |

**Not** taken from ESP-Brookesia, and deliberately avoided: the Lua/JS/WASM/ELF
runtime backends, the ELF loader (P4MiniShell has no SD code execution —
[`ABI.md`](ABI.md) §1), the JSON-UI document model and its LVGL backend (a
second windowing model), the service framework / `lib_utils` / HAL / system
core (duplicates of this project's own layers), and `brookesia_emulation_nes`
(which vendors the GPL-2.0 `nofrendo` emulator — GPL is incompatible with this
MIT project and must not be copied).

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
