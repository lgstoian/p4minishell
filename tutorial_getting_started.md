# Getting Started with P4MiniShell

This tutorial takes you from a fresh checkout to a running device and your first
app. It assumes the reference hardware (ESP32-P4 Function EV Board / JC1060P470C
with an ESP32-C6, JD9165 1024x600 panel, GT911 touch, and a FAT32 microSD card).

For the concepts and the full command list, see [`readme.md`](readme.md) and
[`command.md`](command.md).

---

## 1. What you need

- **Hardware:** the ESP32-P4 board with its C6 co-processor and a FAT32
  microSD card (any size; class 10 recommended).
- **Software:** [ESP-IDF v5.5.5](https://docs.espressif.com/projects/esp-idf/)
  and Python 3 with `pyserial` (the host tools use it).
- **Ports:** the shell console is the native USB-Serial-JTAG port. Note your
  serial port (e.g. `COM3` on Windows, `/dev/ttyACM0` on Linux). Host tools
  accept it as a trailing argument or via the `P4_PORT` environment variable.

## 2. Build and flash the firmware

```powershell
# Source ESP-IDF (adjust the path to your install)
$env:IDF_PATH = "<path-to-esp-idf-v5.5.5>"
. $env:IDF_PATH\export.ps1

# From the repository root (replace <COM_PORT> with your port)
idf.py build
idf.py -p <COM_PORT> flash monitor
```

`idf.py monitor` shows the boot log and then the interactive prompt. Press
`Ctrl+]` to exit the monitor.

> The first build can take several minutes. The build must finish with **zero
> warnings**; treat any warning as a defect.

## 3. First boot

With the SD card inserted, the first boot:

- mounts the card and prints an **"SD card ready"** welcome;
- generates minimal `CONFIG.SYS` and `AUTOEXEC.BAT` files with commented
  defaults;
- shows the prompt, for example `PS \>`.

Without a card the device still boots, but prints a muted "No SD card detected"
message and runs in a reduced in-memory mode.

Try a few commands:

```
help                 show the command summary
help /all            full offline command reference
sysinfo              board, memory, and build summary
about                build identity and license
dir                  list the current directory
mem                  heap usage
```

## 4. Getting around

| Action | How |
|--------|-----|
| Type | on-screen keyboard (tap the input line), USB keyboard, or serial |
| History | input-row `Up`/`Dn`, `Up`/`Down` keys, `Ctrl+R` to search |
| Complete | `Tab` (commands, aliases, paths), or tap the ghost text |
| Scroll | drag the transcript, the input-row `Up`/`Dn` buttons, `PageUp`/`PageDown`, or the mouse wheel |
| Copy/paste | `clip` / `paste` (shared with the `edit` editor) |
| Stop a long command | the input-row **Stop** button or `Ctrl+C` |

The status header shows Wi-Fi, battery, Bluetooth, USB, SD, memory, CPU, and the
clock. Tap an indicator for a one-line detail; long-press to run its status
command.

## 5. Files and the editor

The current directory starts at the SD root. The DOS file verbs work like you
expect:

```
cd APPS
dir
type CONFIG.SYS
edit NOTES.TXT
```

`edit` opens a touch-first full-screen editor. Everything works from the touch
keyboard, a USB keyboard, and the serial console. See
[`tutorial_edit.md`](tutorial_edit.md) for the complete guide.

Handy commands: `mkdir`, `copy`, `move`, `del` (moves to the recycle bin),
`undelete`, `tree`, `find`, `findstr`, `csv`, `json`, `markdown`.

## 6. Run the reference apps

Push the sample apps over serial (the board must be at its prompt,
replace `<COM_PORT>` with your port):

```powershell
python apps/companion/push_sd.py <COM_PORT>   # the Companion system helper
python apps/push_apps.py <COM_PORT>           # tcmd/snake/elite/adventure/...
python apps/push_assets.py <COM_PORT>         # demo BMP sprites
```

Then at the prompt:

```
launch                 menu of discovered apps
launch COMPANION       run the Companion
tcmd                   dual-pane file commander
snake                  the TUI snake game
bounce                 the gfx bouncing-ball demo
```

`launch /list` prints the installed apps with their `APPINFO` titles.

## 7. Install a packaged app

Packages are CRC-checked bundles under `PKGS/<APP>/`. Build and push the sample
bundles, then install from the shell:

```powershell
python apps/push_pkgs.py <COM_PORT>
```

```
pkg list
pkg info SNAKE
pkg install SNAKE
pkg remove SNAKE
```

Installing verifies every payload before copying. Removing sends the app's
files to the recycle bin, so `undelete` can recover it.

## 8. Connect Wi-Fi

```
wifi scan                 list nearby networks (RSSI-sorted)
wifi connect <ssid> <password>
wifi status
wifi save <ssid> <password>   remember a network
wifi known                list remembered networks
```

Once connected, `ping`, `dns`, `httpget`, and `tcpterm` work, and the header
updates. To persist a single credential across boots, set `WIFI_SSID=` and
`WIFI_PASSWORD=` in `CONFIG.SYS` (or use `config`).

## 9. Change the look and feel

```
theme list                built-in palettes (default/amber/ice/mono)
theme set amber /save     switch and persist
font list                 available fonts
header mode auto          responsive status bar
brightness 80
rotate 90
power idle 120            blank the backlight after 2 minutes idle
```

## 10. Get help

- On device: `help <command>` and `help /all`.
- Docs: [`readme.md`](readme.md), [`command.md`](command.md),
  [`roadmap.md`](roadmap.md).
- Something looks broken? Check the **Known quirks** section in
  [`bugs.md`](bugs.md) before reporting.

## Troubleshooting

| Symptom | Likely cause / fix |
|---------|--------------------|
| Black screen but serial prompt answers | You flashed the test app (`p4minishell_tests`). Reflash the main firmware from the repo root. |
| No shell output on serial | Wrong COM port, or the native USB device is absent. Check the cable and Device Manager. |
| "No SD card detected" but the card works | A known early-probe cosmetic issue; the card lazy-mounts on first access. |
| Wi-Fi will not connect | Run `wifi status` and `wifi scan`; check `WIFI_AUTOCONNECT=ON` in `CONFIG.SYS`. |
| A command hung | Press the Stop button / `Ctrl+C`; a background job can be stopped with `taskkill <job>`. |
