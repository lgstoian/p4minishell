# P4MiniShell — Bug Report & Test Campaign

Date: 2026-08-15
Hardware: ESP32-P4 (rev 1.0) on COM11, ESP32-C6 co-processor, JD9165 display, SD card present
Firmware: **v0.31.0** (esp_hosted 3.0.6 + offline help/license/build-identity release)
Scope: Debug sweep and stress testing over the UART console (USB-Serial-JTAG, 115200 baud),
unit suite, and on-board stress runs. Bugs found are ordered by severity; **FIXED** entries
describe the change and its verification.

This campaign starts empty. Every finding from the 0.31.0 sweep is recorded below.

---

## CRITICAL

*(none so far)*

---

## HIGH

*(none so far)*

---

## MEDIUM

*(none so far)*

---

## LOW / OBSERVATIONS

*(none so far)*

---

## NETWORK

### N1. ✅ FIXED — concurrent non-thread-safe `sdmmc_host_init()` wedges the shared SDMMC host at boot

- **Symptoms:** after a `reboot` (or any soft reset), the board occasionally
  failed to bring up BOTH the SD card and the ESP-Hosted C6 on the shared SDMMC
  bus. Boot log showed:
  ```
  E sdmmc_req: sdmmc_host_wait_for_event returned 0x107
  E sdmmc_sd: sdmmc_init_sd_scr: send_scr (1) returned 0x106
  E vfs_fat_sdmmc: sdmmc_card_init failed (0x106).
  No SD card detected - insert a microSD card ...
  E sdmmc_io: sdmmc_io_rw_extended: sdmmc_send_cmd returned 0xffffffff
  E eh_sdio: sdio_read_task: Failed to read data - -1
  [wifi] esp_hosted_connect_to_slave() failed: ERROR (0xffffff8c)
  ```
  Wi-Fi runtime never started that boot. Frequency ~1 in 15 boots; a second
  reboot recovered.
- **Root cause:** the SDMMC host driver is shared between the SD card (slot 0)
  and the ESP-Hosted C6 transport (slot 1). At boot both initialize it from
  different tasks:
  - SD mount: `bsp_sdcard_mount()` → `esp_vfs_fat_sdmmc_mount()` → `host.init()` = `sdmmc_host_init()`
  - Hosted transport: `networking_wifi_runtime_init()` → `esp_hosted_init()` →
    `eh_host_port_sdio_init()` → `sdmmc_host_init()`
  `sdmmc_host_init()` (esp_driver_sdmmc/src/sdmmc_host.c:496) is **not
  thread-safe**: it checks/sets the global `s_host_ctx.intr_handle` without a
  lock, so a concurrent double-call corrupts the host driver state →
  `sdmmc_host_wait_for_event` times out (0x107) and both slots fail.
  Secondary cascade: `esp_vfs_fat_sdmmc_mount` calls whole-host
  `sdmmc_host_deinit()` on any SD mount failure, tearing down the esp_hosted
  slot 1 link even if only the card failed.
- **Fix (3 parts):**
  1. **Serialize host init** — new `storage_sdmmc_host_preinit()` calls
     `sdmmc_host_init()` once, synchronously, from `app_main` before
     `networking_init()`; every later call from fatfs and esp_hosted hits the
     driver's idempotent "already initialized, skip" branch (`sdmmc_host.c:498`)
     so the race can never occur.
  2. **Slot-scoped SD deinit** — patched `bsp_sdcard_mount()` (managed BSP
     `esp32_p4_function_ev_board`) to use a slot-0-only deinit wrapper
     (`sdmmc_host_deinit_slot(0)`) instead of whole-host `sdmmc_host_deinit()`,
     so a failed SD mount / eject only releases slot 0 and never breaks the
     co-processor link (slot 1).
  3. **Hosted bring-up retry** — in `networking_wifi_runtime_init`, if
     `esp_hosted_connect_to_slave()` fails, tear down (`esp_hosted_deinit()`,
     slot-scoped) and retry once after 50 ms.
- **Verified:** 30/30 repeated `reboot` → `wifi connect 4G-CPE_5542
  1234567890` cycles associated and got IP with **zero** hosted-link failures
  (previously ~1/15) and the SD card enumerated every boot. The hosted retry
  never fired (primary fix holds). SD `sdeject` → `wifi status` stays
  "started" (hosted link survives the slot-0 deinit), and `sd mount` re-mounts
  cleanly. Both firmware and test projects build with 0 errors / 0 warnings.

---

## Summary

| Severity | Count | Status |
|----------|-------|--------|
| Critical | 0 | — |
| High | 0 | — |
| Medium | 0 | — |
| Low / observations | 0 | — |
| Network | 1 (N1 shared-SDMMC bring-up) | ✅ Fixed (serialize sdmmc_host_init + slot-scoped SD deinit + hosted retry) |
