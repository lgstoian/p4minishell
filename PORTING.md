# Porting P4MiniShell to a New Board

P4MiniShell keeps all board-specific facts in one **board profile**:
`boards/<name>/board_config.h` (C source of truth) plus its documented twin
`boards/<name>/board_config.yaml`. The reference profile is
`boards/jc1060p470c/` (ESP32-P4 Function EV Board / JC1060P470C, JD9165
1024x600 MIPI-DSI, GT911 touch). A port means adding a profile, pointing the
build at it, and wiring the drivers below — no firmware logic changes.

- **Version:** v1.1.0 · **Target family:** ESP32-P4 + ESP32-C6
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
  (`CONFIG_ESP_HOSTED_HOST_SDIO_PIN_*`, slot, 40 MHz clock); mirror them in
  `BOARD_CFG_HOSTED_SDIO_*` so `gpio map` and the peripheral guards stay
  truthful. `components/command/periph_commands.c` builds its reserved-pin
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
wifi scan         # C6 link alive
python tools/capture_docs.py <COM_PORT>   # eyeball the panel
```

`display stress on` + `tools/display_glitch_watch.py` is the soak gate for
DSI timing changes. Only then run the full suites
(`tools/unit_run.py`, `tools/p4test_run.py`).

## 6. Known next target: M5Stack Tab5 (notes, no code yet)

Same ESP32-P4 + ESP32-C6 SoCs, so toolchain and `sdkconfig` target carry
over. Deltas a future profile must cover: 1280x720 MIPI-DSI panel
(ILI9881C on early units, ST7123/ST7121 integrated display-touch on later —
check the back sticker; ST7123 needs an `esp_lcd_st7123` component that is
**not** vendored yet), ES8388 audio (vs ES8311 — BSP-level swap),
RX8130CE RTC (fits the existing `P4_CONFIG_RTC_EXT_*` external-RTC hook),
SC2356 camera (no camera stack in this workspace — stays an honest error),
and the Tab5Keyboard module (STM32F030, I2C addr `0x6D` + INT pin, Normal /
HID / Character modes) as a new input source beside USB HID and the OSK.
