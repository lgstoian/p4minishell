@echo off
rem SPDX-FileCopyrightText: 2026 Stoian Alexandru
rem SPDX-License-Identifier: MIT
rem =====================================================================
rem  SET.BAT - Preferences. Every setting is owned and persisted by the
rem  firmware's single store, CONFIG.SYS:
rem    config <KEY> <value>          device scalars (brightness, volume, ...)
rem    theme set <name> /save        UI theme
rem    font set|size ... /save       fonts
rem    header mode ... /save         header layout
rem    cursor ... / keyboard mode ...   input prefs
rem    owner ... / security ...      identity, passcode, private records
rem  The app only drives shell commands; it owns no settings file.
rem  Markers: [M-SET] [M-SET-BACK] [M-SET-THEME] [M-SET-THEME-SAVED]
rem           [M-SET-FONT] [M-SET-FONT-SAVED] [M-SET-ROTATE] [M-SET-SEC]
rem  Indices below are 0-based (list returns the selected 0-based index).
rem =====================================================================

:main
echo [M-SET]
draw clear screen
draw box 1 1 80 3 single
draw text 34 2 "PREFERENCES" 14
draw box 1 4 80 16 single "Options"
draw text 5 6 "Settings persist in CONFIG.SYS"
list "Preferences" "Brightness" "Volume" "Rotate" "Idle timeout" "Header" "Theme" "Fonts" "Cursor" "Keyboard page" "System" "Security" "Show settings" "Back"
if errorlevel 255 goto :back
if errorlevel 12 goto :back
if errorlevel 11 goto :show_all
if errorlevel 10 goto :security
if errorlevel 9 goto :system
if errorlevel 8 goto :keyboard
if errorlevel 7 goto :cursor
if errorlevel 6 goto :fonts
if errorlevel 5 goto :theme
if errorlevel 4 goto :header
if errorlevel 3 goto :idle
if errorlevel 2 goto :rotate
if errorlevel 1 goto :volume
goto :brightness

:brightness
draw box 1 1 80 3 single
draw text 33 2 "BRIGHTNESS" 14
ask /v:val "Brightness (0-100)"
if errorlevel 1 goto :main
if "%val%"=="" goto :main
config BRIGHTNESS %val%
if errorlevel 1 dialog "Brightness" "Failed to set brightness." "OK" & goto :main
dialog "Preferences" "Saved brightness=%val%." "OK"
goto :main

:volume
draw box 1 1 80 3 single
draw text 35 2 "VOLUME" 14
ask /v:val "Volume (0-100)"
if errorlevel 1 goto :main
if "%val%"=="" goto :main
config VOLUME %val%
if errorlevel 1 dialog "Volume" "Failed to set volume." "OK" & goto :main
dialog "Preferences" "Saved volume=%val%." "OK"
goto :main

:rotate
echo [M-SET-ROTATE]
draw clear screen
draw box 1 1 80 3 single
draw text 34 2 "ROTATE" 14
list "Rotate" "0" "90" "180" "270" "Back"
if errorlevel 255 goto :main
if errorlevel 4 goto :main
if errorlevel 3 config ROTATE 270 & goto :rot_done
if errorlevel 2 config ROTATE 180 & goto :rot_done
if errorlevel 1 config ROTATE 90 & goto :rot_done
config ROTATE 0
:rot_done
dialog "Preferences" "Rotation saved." "OK"
goto :main

:idle
draw box 1 1 80 3 single
draw text 31 2 "IDLE TIMEOUT" 14
ask /v:val "Display-off after N secs (0 = off)"
if errorlevel 1 goto :main
if "%val%"=="" goto :main
config DISPLAY_TIMEOUT %val%
if errorlevel 1 dialog "Idle" "Failed to set timeout." "OK" & goto :main
dialog "Preferences" "Idle timeout saved." "OK"
goto :main

:header
draw clear screen
draw box 1 1 80 3 single
draw text 35 2 "HEADER" 14
list "Header" "Show" "Hide" "Mode AUTO" "Mode FULL" "Mode COMPACT" "Back"
if errorlevel 255 goto :main
if errorlevel 5 goto :main
if errorlevel 4 header mode compact /save & goto :hdr_done
if errorlevel 3 header mode full /save & goto :hdr_done
if errorlevel 2 header mode auto /save & goto :hdr_done
if errorlevel 1 config HEADER OFF & goto :hdr_done
config HEADER ON
:hdr_done
dialog "Preferences" "Header saved." "OK"
goto :main

:theme
echo [M-SET-THEME]
draw clear screen
draw box 1 1 80 3 single
draw text 36 2 "THEME" 14
list "Theme" "default" "amber" "ice" "mono" "Show" "Back"
if errorlevel 255 goto :main
if errorlevel 5 goto :main
if errorlevel 4 goto :theme_show
if errorlevel 3 theme set mono /save & goto :theme_saved
if errorlevel 2 theme set ice /save & goto :theme_saved
if errorlevel 1 theme set amber /save & goto :theme_saved
theme set default /save
:theme_saved
echo [M-SET-THEME-SAVED]
dialog "Theme" "Theme saved." "OK"
goto :theme

:theme_show
echo [M-SET-THEME-SHOW]
theme show > _theme.txt
view _theme.txt
del /p _theme.txt
goto :theme

:fonts
echo [M-SET-FONT]
draw clear screen
draw box 1 1 80 3 single
draw text 36 2 "FONTS" 14
draw text 5 5 "Terminal is monospace-only; UI takes any font."
list "Fonts" "Terminal font" "UI font" "Terminal size" "UI size" "Show fonts" "Defaults" "Back"
if errorlevel 255 goto :main
if errorlevel 6 goto :main
if errorlevel 5 goto :fonts_default
if errorlevel 4 goto :fonts_show
if errorlevel 3 goto :fonts_uisize
if errorlevel 2 goto :fonts_tsize
if errorlevel 1 goto :fonts_ui
goto :fonts_term

:fonts_term
list "Terminal font" "unscii_16" "DejaVuSansMono" "Back"
if errorlevel 255 goto :fonts
if errorlevel 2 goto :fonts
if errorlevel 1 font set terminal DejaVuSansMono /save & goto :fonts_saved
font set terminal unscii_16 /save
goto :fonts_saved

:fonts_ui
list "UI font" "unscii_16" "montserrat_14" "DejaVuSansMono" "NotoSans-Regular" "Back"
if errorlevel 255 goto :fonts
if errorlevel 4 goto :fonts
if errorlevel 3 font set ui NotoSans-Regular /save & goto :fonts_saved
if errorlevel 2 font set ui DejaVuSansMono /save & goto :fonts_saved
if errorlevel 1 font set ui montserrat_14 /save & goto :fonts_saved
font set ui unscii_16 /save
goto :fonts_saved

:fonts_saved
if errorlevel 1 dialog "Fonts" "Refused (unknown, not monospace, or 80x25 clamp)." "OK" & goto :fonts
echo [M-SET-FONT-SAVED]
dialog "Fonts" "Saved." "OK"
goto :fonts

:fonts_tsize
ask /v:val "Terminal size px (10-20)"
if errorlevel 1 goto :fonts
if "%val%"=="" goto :fonts
font size terminal %val% /save
if errorlevel 1 dialog "Fonts" "Refused (range, bitmap, or 80x25 clamp)." "OK" & goto :fonts
dialog "Fonts" "Saved terminal size=%val%." "OK"
goto :fonts

:fonts_uisize
ask /v:val "UI size px (10-28)"
if errorlevel 1 goto :fonts
if "%val%"=="" goto :fonts
font size ui %val% /save
if errorlevel 1 dialog "Fonts" "Refused (range or bitmap)." "OK" & goto :fonts
dialog "Fonts" "Saved UI size=%val%." "OK"
goto :fonts

:fonts_show
font list > _font.txt
view _font.txt
del /p _font.txt
goto :fonts

:fonts_default
font set terminal unscii_16 /save
font set ui unscii_16 /save
dialog "Fonts" "Back to defaults." "OK"
goto :fonts

:cursor
draw clear screen
draw box 1 1 80 3 single
draw text 35 2 "CURSOR" 14
list "Cursor" "Block" "Bar" "Blink on" "Blink off" "Back"
if errorlevel 255 goto :main
if errorlevel 4 goto :main
if errorlevel 3 cursor blink off & goto :cur_done
if errorlevel 2 cursor blink 500 & goto :cur_done
if errorlevel 1 cursor bar & goto :cur_done
cursor block
:cur_done
dialog "Preferences" "Cursor saved." "OK"
goto :main

:keyboard
draw clear screen
draw box 1 1 80 3 single
draw text 32 2 "KEYBOARD PAGE" 14
list "Keyboard" "text_lower" "text_upper" "number" "symbols" "nav" "nav2" "Back"
if errorlevel 255 goto :main
if errorlevel 6 goto :main
if errorlevel 5 keyboard mode nav2 & goto :kb_done
if errorlevel 4 keyboard mode nav & goto :kb_done
if errorlevel 3 keyboard mode symbols & goto :kb_done
if errorlevel 2 keyboard mode number & goto :kb_done
if errorlevel 1 keyboard mode text_upper & goto :kb_done
keyboard mode text_lower
:kb_done
dialog "Preferences" "Keyboard page saved." "OK"
goto :main

:system
draw clear screen
draw box 1 1 80 3 single
draw text 35 2 "SYSTEM" 14
list "System" "Set prompt" "Launch app at boot" "Wi-Fi autoconnect on" "Wi-Fi autoconnect off" "Back"
if errorlevel 255 goto :main
if errorlevel 4 goto :main
if errorlevel 3 config WIFI_AUTOCONNECT OFF & goto :sys_done
if errorlevel 2 config WIFI_AUTOCONNECT ON & goto :sys_done
if errorlevel 1 goto :sys_launch
ask /v:val "Prompt template (e.g. $p$g)"
if errorlevel 1 goto :system
if "%val%"=="" goto :system
config PROMPT %val%
goto :sys_done
:sys_launch
ask /v:val "App name to offer at boot (blank clears)"
if errorlevel 1 goto :system
config LAUNCH_APP %val%
:sys_done
dialog "Preferences" "System setting saved." "OK"
goto :main

:security
echo [M-SET-SEC]
draw clear screen
draw box 1 1 80 3 single
draw text 34 2 "SECURITY" 14
draw text 5 5 "Owner info, passcode lock, private records."
list "Security" "Owner info" "Conceal: show" "Conceal: mask" "Conceal: hide" "Boot lock on" "Boot lock off" "Set passcode" "Clear passcode" "Lock now" "Back"
if errorlevel 255 goto :main
if errorlevel 10 goto :main
if errorlevel 9 security lock & goto :sec_done
if errorlevel 8 security clearpass & goto :sec_wait
if errorlevel 7 security setpass & goto :sec_wait
if errorlevel 6 security bootlock off & goto :sec_done
if errorlevel 5 security bootlock on & goto :sec_done
if errorlevel 4 security conceal hide & goto :sec_done
if errorlevel 3 security conceal mask & goto :sec_done
if errorlevel 2 security conceal show & goto :sec_done
if errorlevel 1 goto :sec_owner
ask /v:val "Owner name"
if errorlevel 1 goto :security
if not "%val%"=="" owner name %val%
ask /v:val "Owner phone"
if errorlevel 1 goto :security
if not "%val%"=="" owner phone %val%
goto :sec_done
:sec_wait
rem passcode prompts are interactive; no dialog on top of them
goto :security
:sec_done
dialog "Security" "Security setting saved." "OK"
goto :security

:show_all
draw clear screen
draw box 1 1 80 3 single
draw text 33 2 "ALL SETTINGS" 14
config /b > _set.txt
view _set.txt
del /p _set.txt
goto :main

:back
echo [M-SET-BACK]
draw clear screen
goto :eof
