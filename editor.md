# P4MiniShell Editor Guide (v0.35.7 hardware bring-up)

> **Looking for the full reference?** See
> [tutorial_edit.md](tutorial_edit.md) — the complete tutorial covering every
> `edit` feature, edge case, keyboard/serial cheat sheets, and a line-number
> gutter guide. This file is the quick overview.

> **Current build (v0.36.1):** hardware-verified on COM3 (ESP-IDF v5.5.5); unit 262/0/2, deep 8/8, db 38/38, alarm 25/25, smoke 21/21. The editor is one of six modal surfaces on the shared runtime (`dialog`, `list`, `ask`, `browse`, `view`, `hexview`) and shares the 80x25 transcript region; the TUI cell buffer (`components/tui/`), the `draw` verbs, and the `gfx` RGB565 canvas are sibling surfaces.

The `edit` command opens a modal, touch-first text editor that can create and
modify any text file on the SD card — batch scripts, `.txt`, `.sys`, config
files, or any other extension. It is modelled on the classic DOS EDIT
program: full-screen inline editing, a line-number gutter, a current-line
highlight, text selection, cut/copy/paste, find and replace, and a status bar
that always shows where you are.

Everything works from the on-screen touch keyboard, a USB keyboard, or the
serial console.

---

## Opening the editor

```
edit <path>          Opens the file (creates it if it does not exist)
edit                 Opens an unnamed scratch buffer
```

Paths are resolved against the current directory, exactly like `type` or
`copy`. The editor loads the file into RAM (up to
`P4_CONFIG_EDITOR_MAX_BYTES` / `P4_CONFIG_EDITOR_MAX_LINES`). If the file is
larger than those limits the editor refuses to open it and reports an error
instead of silently truncating it.

While the editor is open the shell is paused: the transcript region becomes
the editor surface (exactly the same size as the normal shell transcript), the
input row becomes a status bar, and every input source (USB, touch, serial)
drives the editor. Quitting returns you to the shell. The on-screen keyboard
stays at the bottom of the screen exactly as it does in the shell.

## Editing basics

| Action | Touch keyboard | USB keyboard | Serial console |
|--------|----------------|--------------|----------------|
| Type text | letter keys | letter keys | any line of text |
| Move caret | tap the text | arrow keys | — |
| Page up / down | `PgUp` / `PgDn` | `PageUp` / `PageDown` | — |
| Start / end of line | `Home` / `End` | `Home` / `End` | — |
| Start / end of file | — | `Ctrl+Home` / `Ctrl+End` | — |
| Word left / right | — | `Ctrl+Left` / `Ctrl+Right` | — |
| New line | `Enter` | `Enter` | any line of text |
| Backspace | backspace key | `Backspace` | — |
| Delete forward | `Del` | `Delete` | — |
| Tab (to next stop) | `Tab` | `Tab` | — |
| Insert / overwrite | `Ins` | `Insert` | — |
| Delete whole line | — | `Ctrl+Y` | — |
| Delete to end of line | — | `Ctrl+Shift+End` | — |

The status bar shows the file name, an asterisk when the file has unsaved
changes, the cursor position (`Ln n, Col n`) and the current mode
(`INS` insert / `OVR` overwrite). Overwrite mode replaces the character under
the caret; insert mode pushes the rest of the line right.

A **line-number gutter** on the left shows each line's 1-based number
(right-aligned, `P4_CONFIG_EDITOR_LINE_NUMBER_WIDTH_CHARS` digits) and the
cursor's current line is softly highlighted. The text column therefore starts
just past the gutter; `Go to line` (`Ctrl+G` / `\g` / the nav-page `Goto`)
pairs naturally with it for jumping around long files.

Tabs are expanded to the next tab stop (`P4_CONFIG_EDITOR_TAB_WIDTH`, default
4) so the caret lands on a visual tab stop exactly as it does in DOS EDIT.
Files keep every byte: tabs, 8-bit characters, and the original CRLF vs LF
line-ending style are preserved on save.

## Selection, cut / copy / paste

| Action | Touch keyboard | USB keyboard |
|--------|----------------|--------------|
| Select | long-press then drag over the text | `Shift` + arrow keys |
| Select all | — | `Ctrl+A` |
| Copy | — | `Ctrl+C` |
| Cut | — | `Ctrl+X` |
| Paste | — | `Ctrl+V` |

Selected text is highlighted with a background overlay. Copy and cut use the
same RAM clipboard as the shell's `clip` / `paste` commands, so text you copy
in the editor can be pasted at the shell prompt and vice-versa. Pasting a
multi-line block inserts every line; the EOL style of the pasted text is
converted to the file's own style automatically.

## Find and Replace

The editor has DOS-EDIT style search built into the status bar:

| Action | Touch keyboard | USB keyboard | Serial console |
|--------|----------------|--------------|----------------|
| Find | `Find` | `Ctrl+F` | `\f` |
| Repeat find (next) | `Next` | `F3` | — |
| Replace | `Rep` | `Ctrl+H` | — |
| Go to line | `Goto` | `Ctrl+G` | — |

**Find**: press `Ctrl+F`, type the search text (it appears in the status bar
with a trailing cursor), then press `Enter`. The caret jumps to the next
match, wrapping around the document. Press `F3` (or `Enter` while the search
is still active) to jump to the next match. `Esc` cancels the search.
Search is case-insensitive by default
(`P4_CONFIG_EDITOR_FIND_CASE_SENSITIVE`, default off).

**Replace**: press `Ctrl+H`, type the text to find, `Enter`, then the
replacement text, `Enter`. The first match is replaced and the status bar
reports it. Press `Enter` again to replace the next match; `Esc` stops.
Each replacement is a separate undo step, so you can undo them one at a time.

**Go to line**: press `Ctrl+G`, type the line number, `Enter`. The caret jumps
to that line. Invalid numbers clamp to the file's first/last line.

## Saving and quitting

| Action | Touch keyboard | USB keyboard | Serial console |
|--------|----------------|--------------|----------------|
| Save | `Save` | `Ctrl+S` or `F2` | `\s` |
| Save As | `SaveAs` | `Ctrl+O` | — |
| Quit | `Quit` | `Esc` | `\q` |

- **Save** writes the file back to its original path. An unnamed buffer opens
  the Save-As prompt instead.
- **Save As** prompts for a path (pre-filled with the current one). After a
  successful Save-As the editor's title switches to the new path.
- **Quit** discards the session. If you have unsaved changes the editor asks
  `Quit without saving? (Y/N)` first — press `Y` to discard or `N` to keep
  editing. This guards against losing work by accident.
- A failed save removes the partial destination file and reports
  `save failed`; your edits stay in memory so you can try again.

## Undo / Redo

| Action | Touch keyboard | USB keyboard | Serial console |
|--------|----------------|--------------|----------------|
| Undo | — | `Ctrl+Z` | `\u` |
| Redo | — | `Ctrl+Shift+Z` | `\r` |

The editor keeps `P4_CONFIG_EDITOR_UNDO_DEPTH` full-document snapshots per
session, so undo works across line splits, pastes, replacements, and even
cross-line edits. Note that DOS EDIT had no undo; this is a bonus.

## Touch keyboard layout

The on-screen keyboard has **six pages** — every editor feature is reachable
from the touch keyboard alone:

1. **Letters** (default) — lowercase.
2. **Uppercase** — `ABC`.
3. **Symbols** — every printable ASCII character including the shell-critical
   `|`, `^`, `~`, and `` ` ``.
4. **Nav** — enter it from the symbols page with the `Nav` button. Carries
   `Tab`, `Ins`, `Del`, arrows, `Home`, `End`, `PgUp`, `PgDn`, `Find`, `Next`
   (repeat find), `Rep`, `Goto`, `Undo`, `Redo`, `Save`, `SaveAs`, `Quit`,
   and `Nav2` (-> the Edit page). `abc` returns to the letters page.
5. **Edit** — enter it with `Nav2` on the Nav page. Carries `Copy`, `Cut`,
   `Paste`, `SelAll`, `WdL`/`WdR` (word left/right), `DocH`/`DocE` (document
   home/end), `DelLn` (delete line), `DelE` (delete to end of line), `Nav1`
   (-> the Nav page), and `abc`.

In the shell (editor closed) the same pages work for typing commands; the
`Nav`/`Edit` pages are only available while the editor is open.

## Serial console

While the editor is open, serial lines feed the editor. A backslash-prefixed
verb controls the editor; any other line is typed as text followed by Enter:

| Verb | Action |
|------|--------|
| `\q` or `\quit` | Quit (confirms when there are unsaved changes) |
| `\s` or `\save` | Save |
| `\f` | Find (type text, then Enter) |
| `\g` or `\goto` | Go to line (type the number, then Enter) |
| `\o` or `\saveas` | Save As (type the path, then Enter) |
| `\u` or `\undo` | Undo |
| `\r` or `\redo` | Redo |
| `\a` or `\selectall` | Select all |

## Syntax highlighting

Batch files (`.bat` / `.cmd`) are syntax-highlighted with the built-in batch
lexer: green commands, yellow labels, yellow quoted strings, cyan `%VAR%`
expansions and operators, and grey `rem` / `::` comments. Any other extension
is plain white text. Toggle this with
`P4_CONFIG_EDITOR_SYNTAX_BATCH`.

## Editor limits

| Value | Default |
|-------|---------|
| Maximum file size | `P4_CONFIG_EDITOR_MAX_BYTES` (64 KB) |
| Maximum lines | `P4_CONFIG_EDITOR_MAX_LINES` (2048) |
| Undo depth | `P4_CONFIG_EDITOR_UNDO_DEPTH` (64) |
| Tab width | `P4_CONFIG_EDITOR_TAB_WIDTH` (4) |
| Find / Replace string length | `P4_CONFIG_EDITOR_FIND_BYTES` (128) |

Files beyond the byte/line limits are refused with an error message rather
than truncated, so a save can never silently corrupt them.

## Troubleshooting

- **The shell looks empty after a session** — quitting always restores the
  transcript and jumps it back to the newest output. If you had scrolled up,
  re-scroll with `Up`/`Dn` or press any command.
- **`edit` reports "cannot load"** — the file is larger than the editor
  limits, is not a regular file, or the SD card failed to read. Check the file
  size with `dir` and the card with `sd info`.
- **A save reports `save failed`** — the destination was read-only, the card
  is full, or the write was interrupted. The partial file is removed and your
  in-memory edits survive so you can save to a different path with Save As.
- **Esc quits instead of asking** — Esc only asks for confirmation when the
  document has unsaved changes; an unmodified document quits immediately.

## Implementation notes

The editor lives in `components/editor/`:

- `editor.c` — the byte-preserving document model (lines, cursor, selection,
  undo/redo, find/replace, SD load/save) and the worker-task session.
- `editor_view.c` — the LVGL surface: a dedicated span group with syntax
  colours, a block cursor with blink timer, a selection background overlay,
  and the inline status-bar prompt system used by Find/Replace/Go-to/Save-As
  and the quit confirmation.
- `editor_view.h` — the view's public key/OSK/serial entry points.

The editor reuses the transcript container as its surface
(`windows_enter_editor_mode`), hides the shell's span group while it is open,
and restores it on close. Since v0.33.0 the editor is a surface on the shared
modal runtime in `components/modal/`; input routes through the generic
`shell_command_ops_t.modal_*` hooks (`modal_is_active`,
`modal_handle_usb_key`, `modal_handle_serial_line`), the same ops-table
pattern used by `dialog`, `list`, and `ask`.

> **Note:** `edit` is one `modal_surface_t` among **6** sharing the same runtime
> **Note:** `edit` is one of six `modal_surface_t` surfaces on the shared runtime (`dialog`, `list`, `ask`, `browse`/`filebrowser`, `view`, `hexview`). The TUI cell buffer (`components/tui/tui.h:35` `utf8[4]`, `tui_draw_box`/`tui_draw_line`, `tui_flush` recolor) and the `gfx` RGB565 canvas are separate surfaces reached through the `draw`/`gfx` batch verbs; all render into the live transcript region (80x25) via `windows_enter_tui_mode`/`windows_refresh_tui_surface`, and `draw fullscreen`/`tui fullscreen` hide the header via `windows_set_fullscreen`.
