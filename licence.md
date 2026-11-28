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
are preserved. This includes the P4Sync host CLI/bridge (`tools/p4sync/`) and
the WPF GUI (`apps/p4sync_gui/`): both are project-authored MIT code. They add
**no third-party runtime** — the bridge uses the Python standard library plus
the existing pyserial-backed harness, and the GUI uses only the .NET BCL and
the Windows platform SDK (WPF ships with the SDK; no NuGet packages). No new
attribution entry is required for them.

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
reproduced. The same applies to the explicit retro-MS-DOS visual direction
(80x25 CP437 grid, box-drawn panels, DOS/CGA palette, chunky bitmap cell font,
status bars, keyboard menus) and the DOS-styled web proofs: they are drawn by
this project's own tools and renderer, with no new third-party artwork or font
introduced. Ladybird remains a **BSD-2-Clause design credit only** — no C++/Rust
source was copied — and Lua/MicroPython (MIT) and BMDS (MIT) remain design
credits only (rows below). **Across the v1.6 additive expansion (gfx toolkit, TUI frame and
widgets, modal sessions, HTML5/CSS/JS features and the signed native loader),
no third-party source was copied at all: every new first-party file is
project-authored MIT.** The only external source carried into the tree is code
whose licence file was verified in the upstream repository — the MIT-licensed
BMDS design reference (copyright notice shipped) and the MIT-licensed M5Stack
Tab5 demo drivers listed below; ESP-Brookesia's one derivative tool retains
Apache-2.0. A README claim alone is never treated as a grant. The same
discipline applies to the script-runtime ideas below (Lua
and MicroPython are embeddable languages, deliberately **not** adopted as
runtimes here) and to BMDS:

| Reviewed idea | Licence / copyright | What was taken |
|---------------|---------------------|----------------|
| Lua scripting for handheld apps | MIT, © Lua.org / PUC-Rio | Design credit only: a tiny embeddable scripting surface with explicit host-owned state. No Lua source, interpreter, or binding is shipped; no code copied |
| MicroPython scripting for handheld apps | MIT, © Damien P. George | Design credit only: exposing hardware-adjacent facilities to script-style apps. No MicroPython source, VM, or port is shipped; no code copied |
| [BMDS](https://github.com/jthwho/bmds) | MIT, © 2026 Jason Howard — upstream `LICENSE` verified (`ef05be9f`); pinned notice + text ship at [`tools/LICENSE-BMDS-MIT.txt`](tools/LICENSE-BMDS-MIT.txt) | App-switching/multitasking patterns behind the opt-in `.bat`-launched native payload are **design credit only** (no source copied). **Code reuse under MIT:** `components/gfx3d/gfx3d_math.c` is derived from upstream `sdk/3d/math.c` (column-major 4×4 math, re-expressed in Q16.16; upstream header and `@copyright Jason Howard. All rights reserved.` retained in the file); the DOS shade-ramp in `components/gfx3d/gfx3d_core.c` follows the `sdk/3d/light.c` shade-table **idea only**. No BMDS raster, canvas, mesh, model, GUI, firmware C++ or vendored codec source is used — all pixels still go through the one `gfx_surface` path and one DOS palette. |
| [Ladybird](https://github.com/LadybirdBrowser/ladybird) | **BSD-2-Clause** (not MIT), © 2018-present the Ladybird developers — upstream `LICENSE` verified in the tree | Design credit only: bounded/incremental DOM tokenizing with error recovery, longest-match entity decoding, right-to-left selector matching with rightmost-key bucketing, packed cascade specificity and generation-based style invalidation, arena layout nodes, display-list recording with a structural epoch, bounded PSRAM image budgets, and microtask checkpoints on the one timer pump. This is a C firmware with no MMU or Rust toolchain; no C++/Rust source, table or file was copied, so no upstream licence text is required. Cross-check: [`docs/CRAFT_AUDIT_v143.md`](docs/CRAFT_AUDIT_v143.md) |

Compared with the two browser projects, the implementation deliberately
**surpasses** their limits: we keep QuickJS (neither has JS), a CSS cascade
(babe32 has none), native PNG/JPEG
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

### Host-only vendored verification libraries

These two libraries are used only by host tools and gates. Neither is part of
the firmware or the ESP-IDF build, and neither adds a second decoder or USB
stack to the device.

| Vendored path | Upstream | Licence | What was taken |
|---------------|----------|---------|----------------|
| `tools/thirdparty/miniaudio/` | [miniaudio](https://github.com/mackron/miniaudio) v0.11.26, tag `0.11.26`, commit `877ea4a7c2601c7b0f2a44e83dfac42e6470fea6` | Dual: Public Domain (Unlicense) **or** MIT No Attribution (MIT-0); we take MIT-0. `LICENSE` states `Copyright 2025 David Reid`; the notice inside `miniaudio.h` states `Copyright 2026 David Reid`. Both verbatim texts ship in `tools/thirdparty/miniaudio/`; blob SHAs `2f9423addfe68aa01449ba885d56d832f04b5e8e` (LICENSE) and `b8f61372d97c88b54b97227a5d1d60902726b8e4` (miniaudio.h) were verified against the upstream repository. | The single-header decoder plus a small host helper, compiled only by `tools/host_audio_gate.py` and `tools/audio_asset_prepare.py` to decode WAV/MP3/FLAC assets and cross-check PCM baselines. Not in `components/`, `main/`, any firmware CMake, or `test/`. |
| `tools/thirdparty/lufa/` | [LUFA](https://github.com/abcminiuser/lufa) development build `LUFA_VERSION_STRING "210130"`, commit `90d65ba059d91078a34b9c26c8772ee14b556a13` | LUFA's modified MIT terms, © 2021 Dean Camera. Full text ships verbatim at `tools/thirdparty/lufa/LICENSE.txt`; upstream blob SHA `f63a25f81e33e40029dadc349fc82d6eeea8b28b` verified. This is **not** plain MIT and is **not** SPDX `MIT-advertising` (that text differs), so no SPDX identifier was assumed. | A verbatim subset of named HID constants from `HIDReportData.h`/`HIDClassCommon.h`, plus two small pure descriptor builders re-expressed in our style. Compiled by `tools/thirdparty/lufa/lufa_hid_fixture_check.c` to validate the real HID keyboard constants in `components/usb/usb.c` and the descriptor fixtures in `test/main/test_usb_hid_layout.c`. LUFA is an AVR8 USB stack: no driver, parser, demo or bootloader source is vendored, and nothing LUFA is in firmware or the ESP-IDF build — no second USB stack. |

LUFA's terms require its copyright notice, permission notice and warranty
disclaimer to appear in supporting documentation. The full verbatim text is
reproduced at [`tools/thirdparty/lufa/LICENSE.txt`](tools/thirdparty/lufa/LICENSE.txt)
(and in `tools/thirdparty/lufa/README.md`); the exact notice is:

```text
                  LUFA Library
        Copyright (C) Dean Camera, 2021.

     dean [at] fourwalledcubicle [dot] com
                www.lufa-lib.org


Permission to use, copy, modify, and distribute this software
and its documentation for any purpose is hereby granted without
fee, provided that the above copyright notice appear in all
copies and that both that the copyright notice and this
permission notice and warranty disclaimer appear in supporting
documentation, and that the name of the author not be used in
advertising or publicity pertaining to distribution of the
software without specific, written prior permission.

The author disclaims all warranties with regard to this
software, including all implied warranties of merchantability
and fitness.  In no event shall the author be liable for any
special, indirect or consequential damages or any damages
whatsoever resulting from loss of use, data or profits, whether
in an action of contract, negligence or other tortious action,
arising out of or in connection with the use or performance of
this software.
```

The MIT-0 alternative notice accepted for miniaudio reads:

```text
ALTERNATIVE 2 - MIT No Attribution
===============================================================================
Copyright 2025 David Reid

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

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
| `components/nativepay/` (envelope idea), `.bat`-only payload launch, hybrid app switching | ESP-Brookesia app-manager/multitasking model (native + app pages, per-app context) and BMDS (MIT, © 2026 Jason Howard) small-device app switching; re-expressed with our `start` pool, env overlays, `himem` ownership, shared modal/browser surfaces and shims — no code copied |

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
