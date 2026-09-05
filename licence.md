# Licence Notice (v0.35.2 cleanup, 80×25 `utf8[4]` TUI, stack 24576 at `0x4012b75a`, companion 7 BATs TUI-expanded)

## Project code
Unless a file or directory states otherwise, the original project-specific code in this repository is:

Copyright (c) 2026 Stoian Alexandru
All rights reserved.

No permission is granted to copy, redistribute, sublicense, publish, or use the original project-specific source, documentation, or compiled outputs except with prior written permission from Stoian Alexandru.

This proprietary notice applies to the project-authored parts of the repository, including the shell application logic, ANSI/VT escape sequence module, parser and command-dispatch maintenance fixes, serial-console bridge logic, project documentation, board metadata, command reference files, roadmap notes, and other original files created for P4MiniShell.

## Third-party code
This repository also contains third-party components and dependencies that remain under their own licenses. Those licenses are not replaced by the proprietary notice above.

### Verified third-party licenses in this workspace
- `managed_components/espressif__esp_hosted`: Apache License 2.0
- `managed_components/espressif__esp_wifi_remote`: Apache License 2.0
- `managed_components/espressif__esp_lvgl_port`: Apache License 2.0
- `managed_components/espressif__esp_lcd_jd9165`: Apache License 2.0
- `managed_components/espressif__esp_lcd_touch`: Apache License 2.0
- `managed_components/espressif__esp_lcd_touch_gt911`: Apache License 2.0
- `managed_components/lvgl__lvgl`: MIT License
- `managed_components/espressif__esp_hosted/common/protobuf-c`: BSD-style license from the protobuf-c project

### Project dependencies that should also be respected
- ESP-IDF and Espressif BSP components used by this project are distributed under their own licenses, commonly Apache License 2.0 in the checked-in Espressif components.
- Additional notices inside `managed_components`, `coprocessor`, or future imported components must continue to be preserved exactly as provided by their upstream licensors.

## Distribution rule
Any distribution of this repository or derivative work must preserve:
- this `licence.md` file
- all third-party `LICENSE`, `LICENCE`, `license.txt`, or equivalent notice files shipped with dependencies
- all upstream copyright and attribution notices required by those dependencies

## Scope clarification
This file is a project-level notice only. It is not legal advice, and it does not rewrite the license terms of third-party code already included in the repository.

## Version note (v0.35.2 cleanup patch 0.35.1→0.35.2; hardware baseline v0.35.1 0.35.0→0.35.1)
P4MiniShell v0.35.1 was flashed to COM11, boot verified (`P4MiniShell v0.35.1 ready`, `1024x510` transcript rect `80×25` via `tui status` `p4minishell_config.h:298`), and extensively serial-tested without abort/watchdog/overlap (TUI hardware-verified: `draw box` single/double/rounded with title + nested window stack `tui_draw_box` `components/tui/tui.c:228` via `tui_cell_set` `utf8[4]` `components/tui/tui.h:35` single `SH_BOX_TL`/`H`/`V` double `SH_BOX_TL2`/`H2`/`V2` rounded `SH_BOX_TLR`/`TRR`/`BLR`/`BRR`, `draw line`/`fill`/`text`/`clear`/`window`/`close`/`refresh`/`fullscreen` title+style correctly handled, `draw fullscreen on|off` (global) + `tui fullscreen on|off` (per-app, header kept visible by default `windows_enter_tui_mode` hidden only when fullscreen via `windows_set_fullscreen`/`header_set_visible` `components/windows/windows.c:418` / `tui_enter_fullscreen` `components/tui/tui.c:417`, dynamic keyboard scaling `windows_notify_keyboard_visibility` → `windows_refresh_tui_surface`, TUI does not overlap shell text `tui_hide_for_modal`), `color`/`locate` TUI-aware via `tui_flush` recolor `#RRGGBB` per fg run `ansi_get_palette_color` PowerShell palette no duplicate, prompt `shell_prompt_render_plain()` `main.c:112`/`components/shell/shell.c:412` + `modal_surf.c:412` `keyboard_bind_textarea` situational `SH_PROMPT`, font extended in-place `managed_components/lvgl__lvgl/src/font/lv_font_unscii_16.c` 384 glyphs U+2500-U+257F/U+2600-U+26FF cmaps 3 no duplication `CONFIG_LV_FONT_UNSCII_16=y` `sdkconfig.defaults:33`, screenshot debug `grab_screenshot.py --port/--out/--crop-transcript` + `capture_tui.py`; modal `dialog`/`list`/`ask` with timeout + serial input (`dialog y` `list 2` `ask myname` routed via `shell.c` `modal_handle_serial_line`) + `browse`/`view`/`hexview` all pass, `draw` auto-enters TUI `components/tui/tui.c:56`; memory-pressure fixes `P4_CONFIG_TRANSCRIPT_BYTES` 2048→1024 `p4minishell_config.h:93` / `P4_CONFIG_ASYNC_TRANSCRIPT_BYTES` 1024→512 `p4minishell_config.h:134` / `P4_CONFIG_SD_DMA_BUFFER_BYTES` 8192→4096 `p4minishell_config.h:626` / `P4_CONFIG_TRANSCRIPT_INTERNAL_TRIM_BYTES` 49152→60000 with 1/4 keep + trim-below-10KB `p4minishell_config.h:117` / `P4_CONFIG_COMMAND_TASK_STACK` 16384→24576 `p4minishell_config.h:1514` at `0x4012b75a`, audio `bsp_audio_init` abort guard `components/audio/audio.c:42` `managed_components/espressif__esp_codec_dev/i2s/esp_codec_dev.c:269`, modal `EventGroup` PSRAM `MALLOC_CAP_SPIRAM` `components/modal/modal.c:46`, queue full handling improved, serial routing fix). Bugs M19 memory M20 audio M21 EventGroup M22-M30 TUI M31 stack overflow at `0x4012b75a` all fixed. Companion fully TUI-expanded and hardware-tested via `push_sd.py` COM11 PASS (LIB 1896, COMPANION 1552, SYS 1486, FILES 3946, NET 2893, FUN 3968, SET 3109): 7 BATs (`COMPANION.BAT` draw fullscreen double, `SYS.BAT` tui fullscreen draw boxes, `FILES.BAT` browse/view/hexview + draw + tui fullscreen, `NET.BAT` draw boxes, `FUN.BAT` tui demo, `SET.BAT` tui demo, `LIB.BAT` tui helpers `:tui_banner`/`:tui_header`), extensive serial tests all pass without abort/watchdog/overlap, no regressions.