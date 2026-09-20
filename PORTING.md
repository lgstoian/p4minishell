# Porting P4MiniShell to a New Board

P4MiniShell keeps all board-specific facts in one **board profile**:
`boards/<name>/board_config.h` (C source of truth) plus its documented twin
`boards/<name>/board_config.yaml`. The reference profile is
`boards/jc1060p470c/` (ESP32-P4 Function EV Board / JC1060P470C, JD9165
1024x600 MIPI-DSI, GT911 touch). A port means adding a profile, pointing the
build at it, and wiring the drivers below — no firmware logic changes.

- **Version:** v1.2.0 · **Target family:** ESP32-P4 + ESP32-C6
- Shipped profiles: `boards/jc1060p470c/` (reference) and
  `boards/m5stack_tab5/` (see §6).
- Related: [`readme.md`](readme.md) (configuration), [`SDK.md`](SDK.md)
  (integration), [`ai-context.md`](ai-context.md) (working rules).

## 1. Selecting a profile

```powershell
idf.py -DP4_BOARD=jc1060p470c build        # default, may be omitted
idf.py -DP4_BOARD=myboard build            # uses boards/myboard/
cd test; idf.py -DP4_BOARD=myboard build   # unit-test project mirrors it
```

`P4_BOARD` is a CMake cache var (`CMakeLists.txt`, `test/CMakeLists.txt`)
defaulting to `jc1060p470c`. It stages
`boards/${P4_BOARD}/board_config.h` into the generated config include path;
CI builds every profile under `boards/` (see `.github/workflows/build.yml`).

## 2. Profile anatomy

Copy `boards/jc1060p470c/` and edit. YAML first, then the matching macros:

| YAML section | Header macros | Notes |
|---|---|---|
| `board.i2c` (port, SDA/SCL, clock, pullup) | `BOARD_CFG_I2C_*` | Shared touch + codec + future I2C peripherals; consumed via `BSP_I2C_*` in the patched BSP header |
| `board.i2s` (port, 5 pins, amp GPIO) | `BOARD_CFG_I2S_*`, `BOARD_CFG_POWER_AMP_GPIO` | `components/audio/` only calls `bsp_audio_codec_speaker_init()`; codec differences stay in the BSP |
| `board.display` (geometry, timing, lanes, backlight, reset) | `BOARD_CFG_LCD_*`, `BOARD_CFG_DISPLAY_BRIGHTNESS_LEDC_CH` | `components/display/` builds `bsp_display_cfg_t` from these; see §3 for panel choice |
| `board.touch` (driver, RST/INT, swap/mirror) | `BOARD_CFG_TOUCH_*`, `BOARD_CFG_LCD_TOUCH_*` | Touch INT `NC` disables touch-wake honestly (`sleep` reports it) |
| `board.lvgl` (buffers, rotate, tear) | `BOARD_CFG_LCD_DRAW_BUFFER_*`, `BOARD_CFG_APP_*`, `BOARD_CFG_LVGL_*` | Draw buffer defaults to `width * 50` |
| `board.battery` (ADC GPIO, divider, thresholds) | `BOARD_CFG_BATTERY_*` | Below `PRESENT_MV` the header shows `BAT N/C` instead of 0% |
| `board.rgb_led` | `BOARD_CFG_RGB_LED_GPIO/_IS_WS2812` | Non-WS2812 LEDs need a `components/led/` backend note |
| `board.storage` (mount points **and SDMMC pins**) | `BOARD_CFG_SD_*`, `BOARD_CFG_SD_*_GPIO` | BSP `BSP_SD_*` macros read the pin macros (§4) |
| `board.hosted_sdio` | `BOARD_CFG_HOSTED_SDIO_*` | Mirrors `sdkconfig` `CONFIG_ESP_HOSTED_HOST_SDIO_*`; the C6 reset line stays the `P4_CONFIG_C6_HOST_RESET_GPIO` tunable and must match `CONFIG_ESP_HOSTED_HOST_RESET_GPIO` |

`P4_CONFIG_DISPLAY_PANEL_DRIVER` / `P4_CONFIG_TOUCH_DRIVER`
(`p4minishell_config.h`) are display strings only — update them so
`display info` and `sysinfo` report the new board honestly.

## 3. Display panel and touch

1. Pick the panel branch in the BSP (`esp32_p4_function_ev_board.c` or your
   BSP): JD9165 / ILI9881C / EK79007 / LT8912B are vendored under
   `managed_components/`; anything else needs its `esp_lcd_*` component
   added to root `CMakeLists.txt` `EXTRA_COMPONENT_DIRS` (+ `test/`).
2. Set `BOARD_CFG_LCD_TYPE_*`, geometry, `HSYNC/HBP/HFP/VSYNC/VBP/VFP`,
   `PIXEL_CLOCK`, `DSI_LANE_NUM`, lane bitrates, `COLOR_RGB888`,
   `HW_SWAP_XY` (gates the `esp_lvgl_port` `swap_xy` guard).
3. Set touch RST/INT (or `NC`), `TOUCH_SWAP/MIRROR_*`, I2C clock. A
   non-GT911 touch needs a `bsp_touch_new` equivalent + component dep.
4. Backlight: `BACKLIGHT_USE_LEDC_PWM` + timer/channel/freq/resolution, or a
   plain-GPIO path with a `components/display/` note.

## 4. Storage, SDIO, and pins

- SDMMC data pins are **profile macros now** (`BOARD_CFG_SD_*_GPIO`,
  default 39/40/41/42/44/43); the BSP `BSP_SD_*` block consumes them, and
  every filesystem call uses `BSP_SD_MOUNT_POINT`.
- Hosted-SDIO bus pins stay authoritative in `sdkconfig`
  (`CONFIG_ESP_HOSTED_HOST_SDIO_PIN_*`, slot, clock); mirror them in
  `BOARD_CFG_HOSTED_SDIO_*` so `gpio map` and the peripheral guards stay
  truthful. The clock is per-board: 40 MHz on the reference board, **10 MHz on
  the Tab5** (its first RPC is unreliable at 40 MHz — see §6). `components/command/periph_commands.c` builds its reserved-pin
  table from `BSP_*`/`BOARD_CFG_*` — extend it if your board adds reserved
  lines, so `pwm`/`adc`/`i2c` can never repurpose them.
- After `idf.py update-dependencies`, re-apply
  `powershell -File tools/reapply_managed_patches.ps1` — the uSD-macro hunk
  in `tools/managed_patches.patch` must survive regeneration like the other
  BSP redirects.

## 5. Minimal bring-up checklist

Run on hardware, in order (replace `<COM_PORT>` with your port):

```powershell
idf.py -DP4_BOARD=myboard -p <COM_PORT> flash monitor
# then at the shell:
display info      # geometry, panel, touch, backlight path
gpio map          # reserved pins match your schematic
battery           # sane mV or honest BAT N/C
brightness 80
sd info           # mount + capacity (SD and hosted must coexist)
wifi scan         # C6 link alive (first RPC retried automatically; see §6)
bt enable         # hosted BLE comes up on the C6
python tools/capture_docs.py <COM_PORT>   # eyeball the panel
```

If `wifi scan` reports a dead link, the C6 co-processor firmware does not match
the host `esp_hosted` major — flash a matching slave image first (`c6ota` for a
3.x image, or the one-time standalone flasher for a factory board; see §6).

`display stress on` + `tools/display_glitch_watch.py` is the soak gate for
DSI timing changes. Only then run the full suites
(`tools/unit_run.py`, `tools/p4test_run.py`).

## 6. M5Stack Tab5 profile (`boards/m5stack_tab5/`)

The Tab5 port is implemented and hardware-verified (display, touch, SD, audio,
Wi-Fi, Bluetooth). It shares the ESP32-P4 + ESP32-C6 SoCs with the reference
board, so the toolchain and target carry over; the deltas:

- **Display/touch:** 1280x720 MIPI-DSI, runtime panel auto-detect across
  ILI9881C / ST7123 / ST7121. The board uses the vendored Espressif
  `m5stack_tab5` BSP, trimmed of the camera and IMU/sensor-hub blocks (those
  subsystems live in `components/camera/` and `components/imu/` instead); see
  [`licence.md`](licence.md) for attribution. `BOARD_CFG_LCD_FORCE_VERSION`
  pins a panel version when auto-detect is ambiguous; default rotation is 90.
- **Camera:** SC202CS MIPI-CSI (the board text also says SC2356) through the
  managed `espressif/esp_video` stack, owned by `components/camera/`. `camera
  init` + `camera snap <file.bmp>` write a 24-bit BMP; live preview is future
  work. The sensor rail is gated by `BSP_FEATURE_CAMERA`.
- **IMU:** BMI270 (SYS I2C 0x68) via `components/imu/`; the `imu` command reads
  accel/gyro + orientation and can auto-rotate the display (`imu rotate on`).
- **Audio:** ES8388 codec via the BSP.
- **RTC:** RX8130CE through the `components/clock/` external-RTC hook, selected
  by `BOARD_CFG_RTC_EXT_TIME_REG` (0x10) / `BOARD_CFG_RTC_EXT_KIND_RX8130`
  (2000-based year, STOP bit in control 0x1E).
- **Battery:** INA226 fuel gauge (addr 0x41, 5 mOhm) on the SYS I2C bus, owned
  by `components/power_monitor/`. Measurement is read-only, but charging is
  enabled once at boot (`board_bsp_charge_enable`) via the M5Stack PI4IOE5V6408
  sequence. The shunt reads charge current as **negative**; `battery` shows
  V/A/W/charge, the header shows `+NN%` while charging, and `battery diag` prints
  the raw registers.
- **Input:** the Tab5Keyboard module (STM32F030, expansion I2C 0x6D, INT GPIO50)
  via `components/tab5kbd/`, feeding HID reports into the shell beside USB HID
  and the OSK. Its two RGB LEDs are the board's status LEDs: `components/led/`
  routes its frame to them when `BOARD_CFG_RGB_VIA_TAB5KBD` is set (LED1 =
  status, LED2 = independent user LED via `rgb 2 ...`).
- **Shutdown:** `shutdown`/`poweroff` cuts the PMIC rail by pulsing the
  PI4IOE5V6408 P4 latch (restored in the vendored BSP); the reference board has
  no latch and deep-sleeps instead.
- **Storage/SDIO:** MicroSD on SDMMC slot 0 (pins 39-44); the C6 hosted
  transport shares the SDMMC controller on slot 1. The hosted SDIO clock is
  **10 MHz** here (40 MHz on the reference board).

### Co-processor (C6) firmware

The Tab5 ships with an ESP-Hosted **factory v2.3.0** C6 image that is not wire
compatible with the host `esp_hosted` 3.0.6, so `c6ota` refuses it and an
SD-sourced OTA wedges the shared SDMMC bus. Upgrade the C6 once with a
standalone P4 flasher that carries the slave image in P4 flash (no SD, no
Wi-Fi). The one-time procedure (a throwaway project, not part of the normal
build):

1. Build the matching slave app:
   `cd coprocessor/esp32c6_slave; idf.py set-target esp32c6; idf.py build`
   → `build/esp32c6_hosted_slave.bin`.
2. Make a minimal `esp32p4` project that depends on the same `esp_hosted`
   component, has a raw partition (`slave_fw`, type `data`, subtype `0x40`),
   and calls `esp_hosted_init` → `esp_hosted_connect_to_slave` →
   `esp_hosted_slave_ota_begin`/`_write`/`_end`/`_activate`, streaming the
   app image out of that partition. It must **not** mount the SD card or start
   Wi-Fi (both share the SDMMC controller with the C6 and wedge the transfer).
   Use the Tab5 SDIO pins and a conservative 10 MHz clock.
3. `idf.py -p <COM> flash` the flasher, then write the slave image into the
   partition with esptool (`--force`; the image is a C6 image but the target is
   the P4's flash): `esptool.py -p <COM> write_flash --force <offset> esp32c6_hosted_slave.bin`.
4. Reset; the app runs once, prints progress, and reports
   `*** C6 OTA SUCCESS ***`. Reflash the normal firmware — the C6 keeps the new
   image, after which `c6ota` works normally.

M5Stack's own [M5Tab5-UserDemo](https://github.com/m5stack/M5Tab5-UserDemo)
uses a different Wi-Fi/BT stack (`esp_hosted` 1.4.0 + `esp_wifi_remote` 0.8.5
and its `wifi_c6_fw` binary). This project keeps the project-wide 3.0.6 stack
so a single `components/networking/` drives both boards; the board wiring
(WLAN/USB power and the external-antenna switch through the PI4IOE5V6408
expanders) is identical and already lives in the vendored BSP.

### First-RPC transport reset

On this board the **first hosted RPC issued immediately after a fresh connect**
can time out at the SDIO layer (`sdmmc_send_cmd 0x107`; the slave still holds
DAT0 from its power-on auto-init). `components/networking/networking.c` now
tears the hosted transport down and re-inits/connects once when the C6 version
read fails, which clears the stuck bus and lets Wi-Fi and BLE start. The retry
is failure-only and board-agnostic, so the reference board path is unchanged.
