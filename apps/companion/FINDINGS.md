# P4 Companion — findings log

What the on-board testing of the Companion app surfaced, and what was done
about it. Keep appending here as new bugs/features are found.

## Firmware bugs found and fixed

### 1. Command worker stack overflow (crash) — FIXED
Running `COMPANION.BAT` crashed with a FreeRTOS **stack protection fault**
in the `shell_cmd` task. Root cause: the deep re-entrant batch nesting of
`setlocal` + `call file::routine` + `for /f` over a pipe (each level re-enters
the command executor) overflowed the 12288-byte worker stack by ~580 bytes.
Fix: raised `P4_CONFIG_COMMAND_TASK_STACK` 12288 → 16384 (same reason the
changelog records the earlier 8192 → 12288 raise). The on-screen board has
RAM headroom (heap ~94% free). Verified: the full app runs with no crash.

### 2. `for /f` fails in batch files (malformed options) — FIXED
`for /f "tokens=*" %%v in (file) do ...` printed `for /f: malformed options`
in a batch file, while the identical command worked at the interactive
prompt. Root cause: the interactive tokenizer strips the quotes from the DOS
options string and the rejoin leaves space-separated words, but the batch
path passes the raw line (quotes intact) to the option parser, which then
rejects `"tokens=*"` as an unknown option. This is documented batch syntax
that had never actually been exercised. Fix: strip a leading quote and trim a
trailing quote/space from the options region in `shell_execute_for_loop` so
both paths feed the parser identical text. Verified: batch `for /f` with
`tokens=*`, `tokens=1,2`, etc. now works.

### 3. Key-wait prompt race (pause/choice drop fast input) — FIXED
`pause`, `choice`, the destructive-confirm prompt, the xcopy prompts and the
`-- More --` pagers all printed their prompt text and *then* armed the key
wait (`shell_key_wait_begin()`). The prompt text is queued for the UART
console task, so a key typed as soon as the prompt appears could reach the
console reader before the wait armed — and was then dispatched as a shell
command (or silently dropped) instead of answering the prompt. This made
scripted serial driving of the app flaky (a `pause` would sit its full 10 s
timeout, then the app's default menu action fired). Fix: arm the key wait
*before* printing the prompt in `shell_command_pause`, `shell_command_choice`,
`shell_confirm_destructive`, the three xcopy prompts, and both `-- More --`
pagers. Verified: the deep-test driver (which reacts to prompts the instant
they appear) no longer loses dismiss keys.

### 4. `if COND cmd & other` ran `other` unconditionally — FIXED
The command-chain splitter (`shell_split_chain`) splits a line on unquoted
`&`, `&&` and `||` *before* the command runs. For `if %tries% GEQ 10 echo
Out of tries! & goto main`, that produced two segments — `if ... echo ...`
(SHELL_CHAIN_FIRST) and `goto main` (SHELL_CHAIN_ALWAYS) — so `goto main` ran
on **every** iteration regardless of the condition. This broke the companion's
number-guessing game: after the first guess the batch jumped straight back to
the main menu (which defaulted to the melody). Same hazard for `for` bodies
containing `&`. Fix: `shell_split_chain` now returns the whole line as a
single segment when the first token is `if` or `for`; those commands already
join the rest of the line as their body and re-enter the pipeline, so the `&`
is evaluated only when the condition/iteration runs. Verified: `if ... &
goto` now skips correctly when false; the guessing game plays to "Correct!"
or "Out of tries!".

## New features added (during this exercise)

- **Dynamic pseudo-variables** in batch: `%DATE%` (`MM-DD-YYYY`), `%TIME%`
  (`HH:MM:SS`), `%RANDOM%` (`0..32767`), `%CD%` (current directory). Added in
  `shell_expand_variables()` next to the existing `%ERRORLEVEL%` special case.
- **`delay <ms>`** command — a pure, deterministic wait for melodies/demos
  (unlike `sleep`, which is light-sleep and blanks the display / tears down
  Wi-Fi). Clamped to `P4_CONFIG_DELAY_MAX_MS`.

## Batch-language fixes/hardening added after this first sweep

- **Undefined `%VAR%` now expands to empty** (cmd.exe parity) instead of
  staying literal, so the DOS `if "%var%"==""` idiom works. This removed the
  need for the earlier sentinel workaround in most places (the app still uses
  sentinels, which remain valid and explicit).
- **`if [not] [/i] defined VAR`** added (cmd.exe parity).
- **Command-queue drops**: the worker queue was 4 deep with a 0-timeout
  submit, so rapid serial input silently dropped commands. Depth raised to 8
  and the submit waits up to 1 s. Verified: a burst of 10 rapid commands
  completes with no drops.

## Remaining notes on the batch language (to design around)

- **`set NAME=` still unsets** the variable rather than defining it empty.
  Because undefined `%VAR%` now expands to empty, `if "%NAME%"==""` still
  detects "not set", so this is usually fine; only `if defined NAME` after a
  `set NAME=` differs from cmd.exe (it reports "not defined").
- **`for /f` over a pipe/redirect captures the command's error text too** when
  the source command fails (there is no separate stderr). Guard with the
  command's `errorlevel` before reading: write to a temp file, `if errorlevel
  1 goto :skip`, then `for /f` the file.

## On-board observations (not code bugs)

- **Wi-Fi link to the 4G hotspot is flaky**: the board connects and pings
  fine, but the association drops within ~30–60 s. `wifi known` shows 80+
  reconnects to this AP, so the AP is the likely culprit. The app degrades
  gracefully offline (each network command checks `errorlevel`).
- **`tone` overlaps**: `tone` returns immediately (background playback), so a
  melody must leave a gap (`delay` ≥ tone duration + fade) or the next tone
  reports "audio busy". The melody uses generous gaps now.
- **Transcript trimming** kicks in under internal-RAM pressure (the app +
  modals + menus push internal free down to ~45 KB); the shell's auto-trim
  (v0.32.8) handles it without crashing.
- The `i2s_common: the channel has not been enabled yet` errors during
  `tone` are pre-existing audio-stack noise, not caused by the app.

## Deep-test sweep (`deep_test.py`)

`python deep_test.py COM11` drives every module and branch from a fresh,
uptime-verified reboot each time. Final result: **all 8 checks PASS**
(baseline / menu walk / SYS / FILES / NET offline / FUN / SET / persistence).

Key points learned while building the driver:

- **The board's reset needs the esptool DTR/RTS dance** — plain pyserial RTS
  toggles did not reboot it. `deep_test.py` shells out to `esptool --after=hard_reset`
  and confirms a fresh boot by checking `sysinfo` uptime (< 45 s).
- **Scripted serial driving must be reactive, not paced.** One-key waits
  (`pause`/`choice`/`set /p`) discard input sent before the key queue is armed
  (bug #3 above), so a fixed-pacing driver is inherently flaky. The driver
  reads the stream continuously and reacts to each prompt the instant it
  appears; only the `ask` modal values (which have no serial prompt) use timed
  sends.
- **`appconfig` output is ANSI-coloured**, so plain substring assertions must
  strip escape sequences first (the persist check compares stripped output).
- The trash accumulates quickly when tests run repeatedly (`trash empty`
  before a clean sweep keeps `trash list` fast and the transcript small).

## Later additions (background services, gfx/assets, performance)

- **Background services**: `SVC.BAT` (a headless service loop, started with
  `start SVC`, stopped with `taskkill bg0`) and `AGENDA.BAT` (calendar +
  `notify`) were added, with a **Live System > Services** submenu in `SYS.BAT`
  (Status / Start background service / Stop / List alarms / Run agenda now).
  The companion is now 10 pushed BATs (`push_sd.py`).
- **`start`/`taskkill`** run on a pooled background worker with PSRAM stacks;
  background jobs use headless-safe verbs only (draw/modal/key-wait refuse).
- **`gfx` + assets**: the `BOUNCE.BAT` game (gfx sprites/BMP) and
  `apps/push_assets.py` (PIL sprites + `APPS/<APP>.ASSETS` manifests) round
  out the app story; `asset check` verifies every bundled manifest on device.
- **Performance**: batch files now execute from a 128 KB RAM image (no SD
  reads mid-`goto`-loop), transcript appends are O(1), a `for` loop repaints
  the transcript once, and the serial mirror is skipped when no host is
  attached. Regression after all of it: unit 262/0/2, deep 8/8, db 38/38,
  alarm 25/25, smoke 21/21 (COM3).
