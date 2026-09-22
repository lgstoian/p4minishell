# P4MiniShell — Board Schematics & Hardware Reference

> **READ THIS BEFORE ANY HARDWARE TASK.** This file is the single, detailed
> hardware reference for both supported boards. Every pin, bus, device address,
> power rail, sensor, button, LED, and expansion header is recorded here so no
> agent or developer has to guess from a schematic or re-derive it from code.
> `boards/<name>/board_config.h` / `.yaml` remain the *machine* source of truth
> the firmware compiles; this file is the *human/agent* source of truth and must
> be kept in sync with them. If they disagree, the board_config is authoritative
> for code and this file is a bug to fix.
>
> See the hard rule in [`ai-context.md`](ai-context.md) §0 ("Ground rules").
> Companion docs: [`PORTING.md`](PORTING.md) (board bring-up),
> [`documentation.md`](documentation.md) (modules),
> [`bugs.md`](bugs.md) (findings).

- **Firmware:** v1.2.1 (ESP-IDF v5.5.5)
- **SoC family:** ESP32-P4 (host) + ESP32-C6 (co-processor, ESP-Hosted SDIO)
- **Boards:** `jc1060p470c` (reference) and `m5stack_tab5`
- **Serial:** reference `COM3`, Tab5 `COM6` during development

---

## 0. How to use this file

1. Read the board's section and the **Common** section before touching pins,
   buses, power, display, storage, or any on-board device.
2. `NC` = not connected / not present. A signal on an IO expander is **not** a
   GPIO; it cannot be driven with `gpio`/`pwm`/`adc` and is not in the GPIO
   table. Do not add it to `BOARD_CFG_*_GPIO`.
3. Addresses are 7-bit I2C unless stated. Two boards share a bus name (SYS I2C)
   but **different pins** — always check the board.
4. Anything marked **VERIFY** is not yet confirmed against the vendor schematic;
   confirm before relying on it.

---

## 1. Common architecture

```
        +-----------------------------+          +-------------------------+
        |        ESP32-P4 (host)      |  SDIO    |   ESP32-C6-MINI-1U      |
        |  RISC-V dual-core 360 MHz   |<-------->|  Wi-Fi 6 / BLE / 802.15.4|
        |  16 MB flash, 32 MB PSRAM   | slot 1   |  (ESP-Hosted slave)     |
        +-----------------------------+          +-------------------------+
           |        |         |          |
     MIPI-DSI   I2C(SYS)   SDMMC slot 0  USB
     panel       sensors    microSD       host/OTG
```

- **Co-processor link:** ESP-Hosted over SDMMC **slot 1**, 4-bit. The card
  (slot 0) and the C6 share the SDMMC controller and its DMA-capable internal
  buffers (see `ai-context.md` SD Card Rules; `bugs.md` F6).
- **Wireless stack:** `espressif/esp_hosted` 3.0.6 + `espressif/esp_wifi_remote`
  1.6.2 (only the official path is permitted). The vendor M5Tab5 demo uses
  `esp_hosted` 1.4.0 + `esp_wifi_remote` 0.8.5 — a different pair (see F6).
- **Storage:** microSD on SDMMC slot 0, 4-bit, mount `/sdcard`; SPIFFS
  partition `storage` at `/spiffs`.
- **Display path:** MIPI-DSI, 2 lanes, RGB565; all display ops go through
  `components/display/` (never call the BSP directly from `main.c`).
- **Frame rotation:** both boards use LVGL software rotation
  (`BOARD_CFG_APP_SW_ROTATE=1`), accelerated by the P4 **PPA** 2D engine
  (`CONFIG_LVGL_PORT_ENABLE_PPA=y`).

---

## 2. `jc1060p470c` — ESP32-P4-Function-EV-Board (reference)

**Identity:** requested `JC1060P470C`, detected `ESP32-P4-Function-EV-Board`.
Backed by the managed `espressif__esp32_p4_function_ev_board` BSP.

### 2.1 Features
- ESP32-P4, 16 MB flash, 32 MB PSRAM (200 MHz octal).
- **JD9165** 1024x600 MIPI-DSI panel, **GT911** capacitive touch (I2C).
- **ES8311** audio codec + power amp enable on GPIO20.
- **WS2812** addressable RGB status LED on GPIO26 (back panel).
- Battery sense on **ADC GPIO53** (2:1 divider).
- USB Type-C (USB-Serial/JTAG console + USB OTG), USB-A host.
- microSD (SDMMC slot 0), no RTC, no IMU, no camera.

### 2.2 GPIO pinout

| GPIO | Function | Notes |
|------|----------|-------|
| 7 | I2C SDA (SYS) | shared bus, 400 kHz, internal pull-up |
| 8 | I2C SCL (SYS) | shared bus, 400 kHz |
| 9 | I2S DOUT | to ES8311 |
| 10 | I2S LCLK (WS) | |
| 11 | I2S DSIN | from ES8311 |
| 12 | I2S SCLK (BCLK) | |
| 13 | I2S MCLK | |
| 20 | Audio PA enable | power amplifier |
| 23 | LCD backlight | LEDC PWM, 5 kHz, 10-bit, timer 1, ch 1 |
| 26 | WS2812 RGB data | status LED |
| 27 | LCD reset | |
| 39-42 | SD D0-D3 | SDMMC slot 0 |
| 43 | SD CLK | |
| 44 | SD CMD | |
| 53 | Battery ADC | 2:1 divider, present threshold 2500 mV |
| 14-17 | C6 SDIO D0-D3 | slot 1 (mirrors sdkconfig) |
| 18 | C6 SDIO CLK | 40 MHz |
| 19 | C6 SDIO CMD | |
| 54 | C6 reset | `CONFIG_ESP_HOSTED_HOST_RESET_GPIO` |

### 2.3 Display timing (JD9165)
Native 1024x600, rotation 0. PCLK 80 MHz, HSYNC 1344, HBP 160, HFP 160,
VSYNC 635, VBP 23, VFP 12. 2 DSI lanes, 550 Mbps runtime (macro 1000).
Refresh ~60 Hz (not dynamically changeable). Draw buffer `width*50` = 51200 B,
single-buffered, DMA + PSRAM.

### 2.4 I2C device map (SYS I2C, port 1, SDA 7 / SCL 8)

| Addr | Device | Notes |
|------|--------|-------|
| 0x5D | GT911 touch | reset/INT `NC` (bus-reset via driver) |
| 0x18 | ES8311 codec | **VERIFY** (ES8311 7-bit address) |

### 2.5 Battery
ADC on GPIO53, divider 2:1. Empty 3300 mV, full 4200 mV, present 2500 mV.
Below `present_mv` the input floats → `BAT N/C`.

---

## 3. `m5stack_tab5` — M5Stack Tab5

**Identity:** requested/detected `M5Stack Tab5`. Backed by the official
`espressif/m5stack_tab5` BSP (vendored under `board_bsp/`, trimmed to
display/touch/audio/storage/IO-expander).

### 3.1 Features
- ESP32-P4NRW32 @ 360 MHz + LP core 40 MHz; 16 MB flash, 32 MB octal PSRAM.
- **ESP32-C6-MINI-1U** Wi-Fi 6 / BLE / 802.15.4 co-processor over SDIO.
- 5" IPS **1280x720** MIPI-DSI, integrated TDDI: **ST7123 / ST7121**
  (early units: **ILI9881C** + **GT911**).
- **ES8388** codec + **ES7210** AEC front end, dual mic, **NS4150B** 1 W amp,
  3.5 mm headphone jack.
- **INA226** (0x41) pack monitor + **IP2326** charge management; removable
  **NP-F550** 7.4 V / 2000 mAh 2S pack.
- **RX8130CE** RTC (0x32), **BMI270** IMU (0x68), **SC202CS/SC2356** MIPI-CSI
  camera (0x36).
- **RS-485** (SIT3088), USB-A host + USB-C OTG, microSD, M5-Bus + HY2.0-4P +
  GPIO_EXT, two PI4IOE5V6408 IO expanders.
- **Tab5Keyboard** module (STM32F030, 0x6D) with two RGB LEDs; no on-board
  WS2812.
- Buttons: power (single press on / double press off) and reset/boot.

### 3.2 GPIO pinout

| GPIO | Function | Notes |
|------|----------|-------|
| 0 | Tab5Keyboard SDA | I2C **controller 0** (separate bus) |
| 1 | Tab5Keyboard SCL | I2C controller 0 |
| 8-11 | C6 SDIO D3,D2,D1,D0 | slot 1 (see §3.4) |
| 12 | C6 SDIO CLK | **10 MHz** (not 40) |
| 13 | C6 SDIO CMD | |
| 15 | C6 reset | |
| 22 | LCD backlight | LEDC PWM, 5 kHz, 10-bit, timer 0, ch 1 |
| 23 | Touch interrupt | TDDI INT |
| 26 | I2S DOUT | to ES8388 |
| 27 | I2S SCLK | |
| 28 | I2S DSIN | |
| 29 | I2S LCLK (WS) | |
| 30 | I2S MCLK | |
| 31 | I2C SDA (SYS) | shared bus, 400 kHz |
| 32 | I2C SCL (SYS) | shared bus, 400 kHz |
| 39-42 | SD D0-D3 | SDMMC slot 0 |
| 43 | SD CLK | |
| 44 | SD CMD | |
| 50 | Tab5Keyboard INT | |
| — | LCD reset, TP reset, PA enable | **on IO expanders, not GPIO** |

RS-485 (SIT3088): G20 = TX, G21 = RX, G34 = DIR (**VERIFY** against schematic).

### 3.3 IO expanders (PI4IOE5V6408, SYS I2C)

**E2 — 0x44** (confirmed by `board_bsp/src/bsp_io_expander.c` and M5Stack pinmap):

| Pin | Dir | Signal | Notes |
|-----|-----|--------|-------|
| P0 | out | WLAN_PWR_EN | C6 rail enable |
| P1 | in | (unused) | high-Z |
| P2 | in | (unused) | high-Z |
| P3 | out | USB5V_EN | USB-A 5 V rail |
| P4 | out | PWROFF_PULSE | PMIC power-off latch |
| P5 | out | nCHG_QC_EN | charge QuickCharge enable (active low) |
| P6 | in | **CHG_STAT_LED** | IP2326 charge status (F24-B corroboration) |
| P7 | out | CHG_EN | charge enable (F20) |

Init sequence: `DIR=0xB9`, `OUT_H_IM=0x06`, `PULL_SEL=0xB9`, `PULL_EN=0xF9`,
`IN_DEF_STA=0x40`, `INT_MASK=0xBF`, `OUT_SET=0x89` (P0+P3, then charge gates).
**Ownership (bugs.md F6):** this chip is SINGLE-WRITER — every access goes
through the raw, read-back-verified path in `board_bsp/src/bsp_io_expander.c`
(`bsp_io_expander1_set_output()` + charge/power-off helpers, shadowed OUT byte).
The managed-driver creation path (`bsp_io_expander1_init()`) is never called:
it chip-resets the device and floats P0, power-cycling the C6 mid-boot. P0 must
be driven before esp_hosted enumeration and stay asserted for the session;
esp_hosted's GPIO15 CP-reset pulse is the only sanctioned C6 reset.

**E1 — 0x43** (per M5Stack pinmap; **VERIFY** exact pin→signal before use):

| Pin | Signal | Notes |
|-----|--------|-------|
| P0 | RF_PTH_L_INT_H_EXT | internal/external antenna select (low=int) |
| P1 | RF_INT_EXT_SWITCH | antenna switch |
| P2 | NS4150B SPK_EN | speaker amp enable |
| P4 | EXT_5V_BUS | expansion 5 V rail |
| P5 | EXT5V_EN | expansion 5 V enable |
| P6 | LCD_RST | panel reset |
| P7 | TP_RST | touch reset |
| ? | CAM_RST / HP_DET | camera reset / headphone detect — pin unknown |

### 3.4 Display timing (ST7123/ST7121, native portrait)
Native 720x1280, **rotation 90** (logical 1280x720). PCLK 70 MHz, HSYNC 1360,
HBP 40, HFP 40, VSYNC 730, VBP 8, VFP 220. 2 DSI lanes, 1000 Mbps.
Refresh ~70 Hz (not dynamically changeable). Draw buffer `width*50` = 36000 B,
single-buffered, DMA + PSRAM. Panel auto-detected (probe TDDI at I2C 0x55);
`BOARD_CFG_LCD_FORCE_VERSION` can pin a revision. Screen driver changed
ST7123 → **ST7121** in units after 2026-04-28.

### 3.5 I2C device map (SYS I2C, port 1, SDA 31 / SCL 32)

| Addr | Device | Notes |
|------|--------|-------|
| 0x41 | **INA226** | pack bus voltage / current / power |
| 0x32 | **RX8130CE** | RTC (time block 0x10, 2000-based year, STOP in 0x1E) |
| 0x68 | **BMI270** | 6-axis IMU |
| 0x36 | **SC202CS** | camera SCCB (board text: SC2356) |
| 0x43 | PI4IOE5V6408 E1 | IO expander |
| 0x44 | PI4IOE5V6408 E2 | IO expander (charge/power rails) |
| 0x10 | ES8388 | audio codec |
| 0x40 | ES7210 | AEC front end |
| 0x14 | GT911 | touch (ILI9881C units only) |
| 0x55 | ST7123/ST7121 | integrated TDDI (touch) |

Separate I2C **controller 0** (SDA 0 / SCL 1): **Tab5Keyboard** 0x6D (STM32F030).

### 3.6 Power / battery (INA226 + IP2326)
- **INA226** (0x41), 5 mΩ shunt, max 8192 mA. Read-only: never sets charge
  voltage/current. Bus voltage = **pack rail**; the shunt reads **charge
  current as negative**.
- Pack: 2S Li-ion, empty 6000 mV, full 8400 mV, present 5000 mV (per
  `board_config.h`; the product page quotes full 8.23 V / shutdown 6.0 V).
- **IP2326** charge IC. Charging gated by E2.P7 `CHG_EN` (+ E2.P5
  `nCHG_QC_EN`), enabled once at boot by `board_bsp_charge_enable(true)`.
  Charge status is E2.P6 `CHG_STAT_LED`.
- With **no pack**, the INA226 bus floats to the charger/rail (~8.40 V) with
  ~0 current — indistinguishable from a full pack by voltage alone (F24).
- Power-on: single press power button; power-off: double press; `shutdown`
  darkens the keyboard LEDs then cuts the PMIC rail.

### 3.7 Buttons, LEDs, expansion
- **Buttons:** Power (single on / double off), Reset/Boot (hold ~2 s → download
  mode, green LED flashes).
- **LEDs:** no on-board status LED. The **Tab5Keyboard** module has two
  independently addressable RGB LEDs: LED1 (index 0) = status engine, LED2
  (index 1) = user via `rgb 2 <r> <g> <b>`; driven over I2C via
  `components/tab5kbd/`.
- **Expansion:** M5-Bus, HY2.0-4P (PORT.A: G53/G54), GPIO_EXT, microSD,
  RS-485, 1/4"-20 tripod nut, MMCX external antenna.

---

## 4. Sources & verification

- Machine truth: `boards/<name>/board_config.h` + `board_config.yaml`.
- Tab5: M5Stack Tab5 product docs (`docs.m5stack.com/en/core/Tab5`, pinmap +
  schematic PDFs), `M5Tab5-UserDemo`, `espressif/m5stack_tab5` BSP,
  `board_bsp/src/bsp_io_expander.c`, `bsp_display.c`.
- Reference: `managed_components/espressif__esp32_p4_function_ev_board`,
  `espressif__esp_lcd_jd9165`, `espressif__esp_lcd_touch_gt911`.
- Tab5 schematics: `Tab5_Schematics_PDF.pdf`, `Tab5_Overall_Design_Block_Diagram.pdf`
  (linked from the M5Stack product page).

When a signal is added or changed, update **both** the board_config and this
file in the same change (Documentation Updates rule in `ai-context.md`).
