# P4MiniShell `edit` — Complete Tutorial (v0.35.7 hardware bring-up)

The `edit` command opens a modal, full-screen text editor for any file on the
SD card. It is modelled on the classic MS-DOS `EDIT` program and adds modern
extras: undo/redo, a line-number gutter, a current-line highlight, syntax
highlighting for batch files, and touch support.

> **v0.35.7 hardware bring-up on the v0.35.1 hardware-verified final TUI state:** flashed to COM11, boot verified (`P4MiniShell v0.35.1 ready`, transcript rect `1024x510` `80×25`), extensive serial tests run without abort/watchdog/overlap. TUI engine is live (`P4_CONFIG_TUI_COLS`×`P4_CONFIG_TUI_ROWS` 80×25 via `windows_enter_tui_mode`/`windows_refresh_tui_surface`, `draw` TUI-aware with auto-enter `tui_init`, `tui_draw_box` `utf8[4]` `tui_cell_set` `SH_BOX_*`, `tui_flush` recolor `#RRGGBB` per fg run via `ansi_get_palette_color`, `draw`/`tui fullscreen` header hidden via `windows_set_fullscreen`, prompt `shell_prompt_render_plain()`); modal surfaces `dialog`/`list`/`ask`/`browse`/`view`/`hexview` fill the live transcript region. Font extended in-place `managed_components/lvgl__lvgl/src/font/lv_font_unscii_16.c` 384 glyphs U+2500-U+257F/U+2600-U+26FF cmaps 3 no duplication `CONFIG_LV_FONT_UNSCII_16=y`. Memory fixes (`P4_CONFIG_TRANSCRIPT_BYTES` 2048→1024 `p4minishell_config.h:93`, `P4_CONFIG_ASYNC_TRANSCRIPT_BYTES` 1024→512, `P4_CONFIG_SD_DMA_BUFFER_BYTES` 8192→4096, `P4_CONFIG_TRANSCRIPT_INTERNAL_TRIM_BYTES` 49152→60000 with 1/4 keep), audio `bsp_audio_init` abort guard, modal `EventGroup` PSRAM (`MALLOC_CAP_SPIRAM`), dialog/list/ask serial routing, screenshot debug `grab_screenshot.py --crop-transcript`. Companion fully TUI-expanded and hardware-verified (7 BATs: `COMPANION.BAT` `draw fullscreen` double, `SYS.BAT` `tui fullscreen` with `draw` boxes, `FILES.BAT` `browse`/`view`/`hexview` + `draw` + `tui fullscreen`, `NET.BAT` `draw` boxes, `FUN.BAT` TUI demo, `SET.BAT` TUI demo, `LIB.BAT` tui helpers `:tui_banner`/`:tui_header`) pushed via `push_sd.py` COM11 PASS (LIB 1896, COMPANION 1552, SYS 1486, FILES 3946, NET 2893, FUN 3968, SET 3109), stack overflow at `0x4012b75a` fixed by `P4_CONFIG_COMMAND_TASK_STACK` 16384→24576 `p4minishell_config.h:1514`, no deferred items.

Everything works from three input surfaces — the on-screen touch keyboard, a
USB keyboard, and the serial console — and the editor surface is exactly as
large as the normal shell transcript (the touch keyboard stays at the bottom
of the screen, just like in the shell).

> Quick-reference and implementation notes live in [editor.md](editor.md).
> This tutorial covers every feature in depth.

---

## 1. Starting the editor

```
edit <path>          Open (or create) a file on the SD card
edit                 Open an unnamed scratch buffer
```

- `edit CONFIG.SYS` opens the file from the current directory. Paths resolve
  exactly like `type` or `copy` (`sd:/...`, `/sdcard/...`, or relative).
- If the file does not exist, the editor opens a **new buffer** bound to that
  path; the first `Save` writes it.
- `edit` with no argument opens an **unnamed buffer**. Saving an unnamed
  buffer opens the Save-As prompt so you pick a name.
- The editor loads the whole file into RAM. It **refuses** files larger than
  `P4_CONFIG_EDITOR_MAX_BYTES` (64 KB) or longer than
  `P4_CONFIG_EDITOR_MAX_LINES` (2048 lines) with a clear error — it never
  silently truncates, so a later save can never corrupt a too-big file.

While the editor is open the shell is paused: every keypress, OSK button, and
serial line goes to the editor. Quitting returns you to the shell prompt.

---

## 2. The editor screen

```
┌──────────────────────────────────────────────────────────────┐
│ HEADER  (Wi-Fi / battery / Bluetooth / USB / SD / MEM / CPU) │
├──────────────────────────────────────────────────────────────┤
│    1 line one                                                │
│    2 line two                                                │
│    3 █ line three                                            │
│    4                                                         │
│                                                              │
│        (the editor surface = the normal shell transcript)    │
├──────────────────────────────────────────────────────────────┤
│ TEST.TXT  *  Ln 3, Col 6  INS                    ← status bar│
├──────────────────────────────────────────────────────────────┤
│               (on-screen keyboard, at the bottom)            │
└──────────────────────────────────────────────────────────────┘
```

- **Line-number gutter** (left): every line is prefixed with its 1-based line
  number, right-aligned in a fixed-width gutter (`P4_CONFIG_EDITOR_LINE_NUMBER_WIDTH_CHARS`,
  default 4 digits) followed by a space. The gutter is muted grey and never
  part of the document.
- **Text area**: the current line is softly highlighted
  (`P4_CONFIG_EDITOR_CURRENT_LINE`); the blinking block caret marks your
  position; a background overlay highlights any selection.
- **Status bar** (the former input row): file name, an asterisk `*` when the
  buffer has unsaved changes, `Ln n, Col n` (1-based line and column), and
  `INS` / `OVR` for insert vs overwrite mode. Prompts (Find / Replace /
  Go-to-Line / Save As / Quit confirm) also appear here.
- **On-screen keyboard**: stays at the bottom, exactly as in the shell.

---

## 3. Typing and basic editing

| Action | Touch keyboard | USB keyboard | Serial console |
|--------|----------------|--------------|----------------|
| Type a character | letter/symbol keys | letter keys | a line of text |
| New line | `Enter` | `Enter` | a line of text |
| Backspace | backspace key | `Backspace` | — |
| Delete forward | `Del` (Nav page) | `Delete` | — |
| Tab (to next stop) | `Tab` (Nav page) | `Tab` | — |
| Insert / overwrite | `Ins` (Nav page) | `Insert` | — |
| Delete whole line | `DelLn` (Edit page) | `Ctrl+Y` | — |
| Delete to end of line | `DelE` (Edit page) | `Ctrl+Shift+End` | — |

- **Insert mode (default)**: typing pushes the rest of the line to the right.
- **Overwrite mode**: typing replaces the character under the caret. Toggle
  with `Insert` / `Ins`; the status bar shows `OVR`.
- **Tab** inserts spaces up to the next tab stop
  (`P4_CONFIG_EDITOR_TAB_WIDTH`, default 4) so the caret lands on a visual tab
  stop, exactly like DOS EDIT. Tab characters already in the file are
  preserved byte-for-byte.
- **Delete whole line** (`Ctrl+Y`) removes the current line and joins its
  neighbours. Deleting the only line leaves one empty line.
- **Delete to end of line** (`Ctrl+Shift+End`) trims everything from the
  caret to the end of the line.
- **Backspace at the start of a line** joins it with the previous line;
  **Delete at the end of a line** joins it with the next line.
- Files keep every byte: tabs, 8-bit characters, and the original CRLF vs LF
  line-ending style are preserved on save (see §10).

---

## 4. Moving the caret

| Action | Touch keyboard | USB keyboard | Serial console |
|--------|----------------|--------------|----------------|
| Place caret | tap the text | click (mouse) | — |
| Left / right / up / down | nav arrows | arrow keys | — |
| Start / end of line | `Home` / `End` (Nav page) | `Home` / `End` | — |
| One word left / right | `WdL` / `WdR` (Edit page) | `Ctrl+Left` / `Ctrl+Right` | — |
| Start / end of document | `DocH` / `DocE` (Edit page) | `Ctrl+Home` / `Ctrl+End` | — |
| One page up / down | `PgUp` / `PgDn` (Nav page) | `PageUp` / `PageDown` | — |
| Go to a line | `Goto` (Nav page) | `Ctrl+G` | `\g` |

- **Go to line**: press `Ctrl+G` (or `\g`, or the nav-page `Goto`), type the
  1-based line number into the status bar, press `Enter`. Numbers outside the
  document clamp to the first/last line. This is the fastest way to jump in a
  long file, especially with the line-number gutter as a guide.
- The caret **stays on screen**: typing or moving below/above the visible area
  scrolls the view so the caret row is always visible.
- **Mouse wheel** scrolls the editor without moving the caret.

---

## 5. Selecting text

| Action | Touch keyboard | USB keyboard |
|--------|----------------|--------------|
| Select a range | long-press, then drag | `Shift` + arrow keys |
| Select all | `SelAll` (Edit page) | `Ctrl+A` |

The selected range is highlighted with a background overlay. Combined with
§3's editing keys you can delete a selection with Backspace/Delete/Enter, or
type to replace it.

---

## 6. Clipboard: copy, cut, paste

| Action | Touch keyboard | USB keyboard |
|--------|----------------|--------------|
| Copy | `Copy` (Edit page) | `Ctrl+C` |
| Cut | `Cut` (Edit page) | `Ctrl+X` |
| Paste | `Paste` (Edit page) | `Ctrl+V` |

- Copy/cut use the **same RAM clipboard** as the shell's `clip` / `paste`
  commands, so text copied in the editor can be pasted at the shell prompt
  and vice-versa.
- Pasting a multi-line block inserts every line; the EOL style of the pasted
  text is converted to the file's own style automatically.
- Paste is a single undo step even for multi-line blocks.

---

## 7. Find, Replace, and Go to line

The search prompts live in the status bar.

| Action | Touch keyboard | USB keyboard | Serial console |
|--------|----------------|--------------|----------------|
| Find | `Find` (Nav page) | `Ctrl+F` | `\f` |
| Repeat / find next | `Next` (Nav page) | `F3` | — |
| Replace | `Rep` (Nav page) | `Ctrl+H` | — |
| Go to line | `Goto` (Nav page) | `Ctrl+G` | `\g` |

**Find** — press `Ctrl+F` (or `\f` / the `Find` button), type the search text
into the status bar, press `Enter`. The caret jumps to the next match,
wrapping around the document. `F3` (or `Enter` while the search is active)
jumps to the next match. `Esc` cancels. Search is case-insensitive by default
(`P4_CONFIG_EDITOR_FIND_CASE_SENSITIVE`, default off).

**Replace** — press `Ctrl+H` (or the `Rep` button), type the text to find,
`Enter`, then the replacement text, `Enter`. The first match is replaced and
the status bar reports it; press `Enter` again to replace the next match;
`Esc` stops. Each replacement is its own undo step, so you can undo them one
at a time.

**Go to line** — press `Ctrl+G` (or `\g` / the `Goto` button), type the line
number, `Enter`. See §4.

---

## 8. Undo and redo

| Action | Touch keyboard | USB keyboard | Serial console |
|--------|----------------|--------------|----------------|
| Undo | `Undo` (Nav page) | `Ctrl+Z` | `\u` |
| Redo | `Redo` (Nav page) | `Ctrl+Shift+Z` | `\r` |

- The editor keeps `P4_CONFIG_EDITOR_UNDO_DEPTH` (64) full-document snapshots,
  so undo works across line splits, pastes, replacements, and cross-line
  edits — far more powerful than DOS EDIT's lack of undo.
- Undo restores the cursor and selection too.

---

## 9. Saving and quitting

| Action | Touch keyboard | USB keyboard | Serial console |
|--------|----------------|--------------|----------------|
| Save | `Save` | `Ctrl+S` or `F2` | `\s` |
| Save As | `SaveAs` | `Ctrl+O` | `\o` |
| Quit | `Quit` | `Esc` (or `Ctrl+Q`) | `\q` |

- **Save** writes back to the original path. Unnamed buffers open the Save-As
  prompt instead.
- **Save As** (`Ctrl+O` / `\o` / the `SaveAs` button) prompts for a new path,
  pre-filled with the current one. Type the new path and press `Enter`. After
  a successful Save As the status bar retitles to the new path.
- **Quit** ends the session. If the buffer has unsaved changes the editor asks
  `Quit without saving? (Y/N)` — `Y` discards, `N` keeps editing. A clean
  buffer quits immediately.
- If a save fails (read-only destination, full card, write error) the partial
  destination file is removed and the status bar reports `save failed`; your
  edits stay in memory so you can retry or Save As to another path.

---

## 10. Line endings and byte preservation

- On load the editor detects CRLF (`\r\n`) vs bare-LF (`\n`) line endings and
  remembers which one the file used.
- On save it writes the file with the **same** style; a file that ended with a
  trailing newline keeps it, one that did not does not gain one.
- Tabs, 8-bit bytes, and long lines are kept byte-for-byte. Very long lines
  are clipped at the right edge of the editor (they never wrap), exactly like
  DOS EDIT.

---

## 11. Syntax highlighting

Batch files (`.bat` / `.cmd`) are highlighted by the built-in batch lexer
(`P4_CONFIG_EDITOR_SYNTAX_BATCH`): green commands, yellow labels and quoted
strings, cyan `%VAR%` expansions and `& | < > ^` operators, and grey
`rem` / `::` comments. Any other extension is plain white text.

---

## 12. Touch keyboard pages

The on-screen keyboard has **six pages** — every `edit` feature is reachable
from the touch keyboard alone:

1. **Letters** (default) — lowercase `a`–`z`.
2. **Uppercase** — `ABC`.
3. **Symbols** — every printable ASCII character, including the shell-critical
   `|`, `^`, `~`, and `` ` ``.
4. **Nav** — press `Nav` on the symbols page. Carries navigation, file, and
   search commands.
5. **Edit** — press `Nav2` on the Nav page. Carries clipboard and advanced
   editing commands.
6. Text pages again — `abc` (on either nav page) returns to the letters page.

**Nav page (page 1):**

| Row | Buttons |
|-----|---------|
| 1 | `Tab`, `Up`, `Home`, `Del`, `Ins`, `Find` |
| 2 | `Left`, `Down`, `Right`, `End`, `PgUp`, `PgDn` |
| 3 | `Undo`, `Redo`, `Rep`, `Goto`, `Save`, `SaveAs` |
| 4 | `Quit`, `Next`, `Nav2`, `abc` |

**Edit page (page 2):**

| Row | Buttons |
|-----|---------|
| 1 | `Copy`, `Cut`, `Paste`, `SelAll`, `WdL`, `WdR` |
| 2 | `DocH`, `DocE`, `DelLn`, `DelE`, `Nav1`, `abc` |

Legend: `WdL`/`WdR` = word left/right, `DocH`/`DocE` = document home/end,
`DelLn` = delete whole line, `DelE` = delete to end of line, `SelAll` =
select all, `Next` = repeat the last find (next match). `Nav1`/`Nav2` switch
between the two nav pages; `abc` returns to the letters page.

In the shell (editor closed) the same pages type commands; the Nav/Edit pages
are only available while the editor is open.

---

## 13. USB keyboard reference

| Keys | Action |
|------|--------|
| Letters / digits / symbols | Insert character |
| `Enter` | New line (or confirm a prompt) |
| `Backspace` | Delete character before the caret |
| `Delete` | Delete character at the caret |
| `Tab` | Spaces to the next tab stop |
| `Insert` | Toggle insert / overwrite |
| Arrow keys | Move caret |
| `Shift` + arrows | Select |
| `Home` / `End` | Start / end of line |
| `Ctrl+Left` / `Ctrl+Right` | One word left / right |
| `Ctrl+Home` / `Ctrl+End` | Start / end of document |
| `PageUp` / `PageDown` | One page up / down |
| `Ctrl+A` | Select all |
| `Ctrl+C` / `Ctrl+X` / `Ctrl+V` | Copy / cut / paste |
| `Ctrl+Z` | Undo |
| `Ctrl+Shift+Z` | Redo |
| `Ctrl+Y` | Delete whole line |
| `Ctrl+Shift+End` | Delete to end of line |
| `Ctrl+F` | Find |
| `F3` | Repeat find (next match) |
| `Ctrl+H` | Replace |
| `Ctrl+G` | Go to line |
| `Ctrl+S` / `F2` | Save |
| `Ctrl+O` | Save As |
| `Esc` / `Ctrl+Q` | Quit (confirms if modified) |

---

## 14. Serial console reference

While the editor is open, every serial line feeds the editor. A
backslash-prefixed verb controls it; anything else is typed as text followed
by `Enter`.

| Verb | Action |
|------|--------|
| `\q` or `\quit` | Quit (asks Y/N when there are unsaved changes) |
| `\s` or `\save` | Save |
| `\f` or `\find` | Find (then type the text and `Enter`) |
| `\g` or `\goto` | Go to line (then type the number and `Enter`) |
| `\o` or `\saveas` | Save As (then type the path and `Enter`) |
| `\u` or `\undo` | Undo |
| `\r` or `\redo` | Redo |
| `\a` or `\selectall` | Select all |

Example serial session:

```
edit TEST.TXT          <- open (new buffer)
line one               <- typed, then Enter
line two
\s                     <- save
\g                     <- goto prompt
1
\q                     <- quit (clean, quits immediately)
type TEST.TXT          <- verify: line one / line two
```

---

## 15. Editor limits

| Value | Default |
|-------|---------|
| Maximum file size | `P4_CONFIG_EDITOR_MAX_BYTES` (64 KB) |
| Maximum lines | `P4_CONFIG_EDITOR_MAX_LINES` (2048) |
| Undo depth | `P4_CONFIG_EDITOR_UNDO_DEPTH` (64) |
| Tab width | `P4_CONFIG_EDITOR_TAB_WIDTH` (4) |
| Find / Replace string length | `P4_CONFIG_EDITOR_FIND_BYTES` (128) |
| Line-number gutter width | `P4_CONFIG_EDITOR_LINE_NUMBER_WIDTH_CHARS` (4) |

Files beyond the byte/line limits are refused with an error, never truncated.

---

## 16. Edge cases and tips

- **Editing at the start of a line with serial**: a serial line is *text plus
  Enter*, so typing at the start of an existing line splits it at the caret.
  This is expected DOS-EDIT-like behaviour.
- **Blank serial line**: an empty line (just `Enter`) during an edit is
  ignored — it does not insert a phantom blank line.
- **Unnamed buffer**: save prompts for a path; nothing is written until you
  provide one.
- **Modified file**: `Esc` / `\q` / `Quit` ask `Quit without saving? (Y/N)`;
  answer `N` to keep working.
- **`edit` a directory or a too-big file**: refused with an error message; the
  shell stays intact.
- **Copy from the editor to the shell**: select text, `Ctrl+C`, quit, then
  press `Ctrl+V` at the prompt (or use `paste`).
- **Long lines**: clipped visually, preserved on disk.
- **Many files / repeated edits**: each session is independent; a crash or a
  rotation during editing closes cleanly without corrupting the file.

---

## 17. Troubleshooting

- **The shell looks empty after a session** — quitting restores the transcript
  and jumps it to the newest output; scroll with `Up`/`Dn` if needed.
- **`edit` reports "cannot load"** — the file is larger than the editor
  limits, is not a regular file, or the SD card failed. Check `dir` / `sd info`.
- **A save reports `save failed`** — read-only destination, full card, or an
  interrupted write. The partial file is removed; your edits survive, so Save
  As to a different path.
- **The caret is not where you expect** — it is measured from the document
  column (past the line-number gutter); tapping in the gutter places it at
  column 1.
- **Find "not found"** — the search is case-insensitive by default; check the
  text for trailing spaces or punctuation.

---

## 18. Implementation notes

The editor lives in `components/editor/`:

- `editor.c` — the byte-preserving document model (lines, cursor, selection,
  undo/redo, find/replace, the pure line-number gutter formatter, SD
  load/save) and the worker-task session.
- `editor_view.c` — the LVGL surface: syntax-coloured spans with a
  line-number gutter, a current-line highlight, a blinking block cursor, a
  selection overlay, and the status-bar prompt system (Find/Replace/Go-to/
  Save-As/quit confirm).
- `editor_view.h` — the view's public key/OSK/serial entry points.

The editor reuses the transcript container as its surface
(`windows_enter_editor_mode`), keeps it visible at the transcript-region
height (so it is exactly the size of the shell transcript), hides the shell's
span group while open, and restores it on close. Since v0.33.0 the editor is a
surface on the shared modal runtime in `components/modal/`; input routes
through the generic `shell_command_ops_t.modal_*` hooks (`modal_is_active`,
`modal_handle_usb_key`, `modal_handle_serial_line`) — the same ops-table
pattern used by `dialog`, `list`, and `ask`.

> **Other modals:** `edit` is one of **6** `modal_surface_t` surfaces plus the TUI cell buffer (`components/tui/tui.h:35` `utf8[4]`, `tui_draw_box`/`line` via `tui_cell_set`, `tui_flush` recolor) sharing the same runtime — `dialog`, `list`, `ask`, `browse`/`filebrowser`, `view`, `hexview` — all via `windows_enter_tui_mode`/`windows_refresh_tui_surface`/`windows_notify_keyboard_visibility` onto the live transcript region (`1024x510` `80×25` `tui status`, `sdkconfig.defaults:33` `CONFIG_LV_FONT_UNSCII_16=y` 384 glyphs U+2500-U+257F/U+2600-U+26FF, `draw fullscreen`/`tui fullscreen` header hidden via `windows_set_fullscreen`, prompt `shell_prompt_render_plain()`), hardware-verified on COM11 (`draw box single/double/rounded` with title + nested window stack, `draw line`/`fill`/`text`, `dialog`/`list` timeout + serial input, `ask` serial, `browse`/`view`/`hexview` fill transcript region, `color`/`locate` per-fg recolor, screenshot `grab_screenshot.py --crop-transcript`). All accept `/t:secs` (auto-cancel) and `/v:NAME` (result variable).
> See `documentation.md` “TUI Module” and `SDK.md` “TUI Integration”. Companion fully TUI-expanded and hardware-verified (7 BATs: `COMPANION.BAT` `draw fullscreen` double, `SYS.BAT` `tui fullscreen` with `draw` boxes, `FILES.BAT` `browse`/`view`/`hexview` + `draw` + `tui fullscreen`, `NET.BAT` `draw` boxes, `FUN.BAT` TUI demo, `SET.BAT` TUI demo, `LIB.BAT` tui helpers `:tui_banner`/`:tui_header`) pushed via `push_sd.py` COM11 PASS (LIB 1896, COMPANION 1552, SYS 1486, FILES 3946, NET 2893, FUN 3968, SET 3109), stack overflow at `0x4012b75a` fixed by `P4_CONFIG_COMMAND_TASK_STACK` 16384→24576 `p4minishell_config.h:1514`, no deferred items.

All tunables are `P4_CONFIG_EDITOR_*` and documented in
`p4minishell_config.yaml`.
