# P4MiniShell `edit` - Complete Tutorial

The `edit` command opens a modal, full-screen text editor for any file on the
SD card. It is modelled on the classic MS-DOS `EDIT` program and adds modern
extras: undo/redo, a line-number gutter, a current-line highlight, syntax
highlighting for batch files, and touch support.

> **Current build (v1.1.0):** hardware-verified on COM3 (ESP-IDF v5.5.5). The
> editor is one of six modal surfaces on the shared runtime (`dialog`, `list`,
> `ask`, `browse`, `view`, `hexview`) and shares the 80x25 transcript region;
> the TUI cell buffer (`components/tui/`), the `draw` verbs, and the `gfx`
> RGB565 canvas are sibling surfaces. Test baselines live in
> [`test/README.md`](test/README.md).

Everything works from three input surfaces — the on-screen touch keyboard, a
physical keyboard (USB HID, a connected Bluetooth HID keyboard, or the M5Stack
Tab5 keyboard; any of these auto-hides the on-screen keyboard), and the serial
console — and the editor surface is exactly as large as the normal shell
transcript (the touch keyboard stays at the bottom of the screen, just like in
the shell).

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
- The editor loads the whole file into PSRAM. It **refuses** files larger than
  `P4_CONFIG_EDITOR_MAX_BYTES` (1 MB) or longer than
  `P4_CONFIG_EDITOR_MAX_LINES` (65 536 lines) with a clear error — it never
  silently truncates, so a later save can never corrupt a too-big file.

While the editor is open the shell is paused: every keypress, OSK button, and
serial line goes to the editor. Quitting returns you to the shell prompt. The
on-screen keyboard opens directly on the **Nav** page (every editor button is
reachable by touch from the first frame); `abc` returns to the letters page.

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
| Open | `Open` | `F4` | `\open` |
| Quit | `Quit` | `Esc` (or `Ctrl+Q`) | `\q` |

- **Save** writes back to the original path. Unnamed buffers open the Save-As
  prompt instead.
- **Save As** (`Ctrl+O` / `\o` / the `SaveAs` button) prompts for a new path,
  pre-filled with the current one. Type the new path and press `Enter`. After
  a successful Save As the status bar retitles to the new path.
- **Open** (`F4` / `\open` / the `Open` button) switches to another file. It
  prompts for a path, pre-filled with the current one; a dirty buffer first asks
  `Open without saving? (Y/N)`. A missing path opens a new empty buffer bound to
  it, so `Open` is also how you create a file without leaving the editor.
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
from the touch keyboard alone. The editor **opens on the Nav page**:

1. **Letters** (default) — lowercase `a`–`z`.
2. **Uppercase** — `ABC`.
3. **Symbols** — every printable ASCII character, including the shell-critical
   `|`, `^`, `~`, and `` ` ``; its `Nav` button switches to the Nav page.
4. **Nav** — the editor control page (uniform 4×6). Carries navigation, file,
   and search commands.
5. **Edit** — press `Edit` on the Nav page (uniform 3×6). Carries clipboard,
   advanced editing, and text-tool commands.
6. Text pages again — `abc` (on either nav page) returns to the letters page.

**Nav page (page 1, 4×6):**

| Row | Buttons |
|-----|---------|
| 1 | `Tab`, `Left`, `Up`, `Down`, `Right`, `Home` |
| 2 | `End`, `PgUp`, `PgDn`, `Del`, `Ins`, `Undo` |
| 3 | `Redo`, `Find`, `Next`, `Replace`, `ReplAll`, `Case` |
| 4 | `Goto`, `Save`, `SaveAs`, `Open`, `Edit`, `abc` |

**Edit page (page 2, 3×6):**

| Row | Buttons |
|-----|---------|
| 1 | `Copy`, `Cut`, `Paste`, `SelAll`, `WordL`, `WordR` |
| 2 | `DocTop`, `DocBot`, `DelLine`, `DelEOL`, `Reload`, `Quit` |
| 3 | `Preview`, `Comment`, `Match`, `Wrap`, `Nav`, `abc` |

Legend: `WordL`/`WordR` = word left/right, `DocTop`/`DocBot` = document
home/end, `DelLine` = delete whole line, `DelEOL` = delete to end of line,
`SelAll` = select all, `Replace` = search and replace, `ReplAll` = replace all,
`Case` = toggle case, `Preview` = Markdown preview, `Next` = repeat the last
find, `Comment`/`Match`/`Wrap`/`Reload` = toggle line comment, jump to matching
bracket, toggle word wrap, reload from disk. `Nav`/`Edit` switch between the two
action pages; `abc` returns to the letters page.

When a prompt asks for text (Find, Replace, Go to line, Save As, Open) the
keyboard automatically switches to the letters page; committing or cancelling
returns it to the letters page (the editor's default page).

In the shell (editor closed) the same pages type commands; the Nav/Edit pages
are only available while the editor is open. The shell input row also has a `Tab`
button that performs the same completion as the USB `Tab` key.

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
| `F4` | Open another file |
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
| `\o` or `\saveas` | Save As (then type the path and `Enter`) |
| `\open` | Open another file (then type the path and `Enter`) |
| `\f` or `\find` | Find (then type the text and `Enter`) |
| `\g` or `\goto` | Go to line (then type the number and `Enter`) |
| `\u` or `\undo` | Undo |
| `\r` or `\redo` | Redo |
| `\all` or `\replaceall` | Replace all |
| `\c` or `\case` | Toggle case |
| `\b` or `\match` | Jump to the matching bracket |
| `\co` or `\comment` | Toggle the line comment |
| `\w` or `\wrap` | Toggle word wrap |
| `\l` or `\reload` | Reload from disk |
| `\p` or `\preview` | Toggle preview |
| `\a` or `\selectall` | Select all |
| `\focus` | Toggle focus / typewriter mode |
| `\spell` | Toggle spellcheck underlines |

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

## 15. Focus, spellcheck, templates, and word count (writerdeck)

Writing-oriented extras, all opt-in and non-destructive:

- **Focus / typewriter mode** — `edit NOTES.TXT /focus` hides the header and
  the on-screen keyboard and keeps the caret vertically centred so text does
  not drift toward the bottom edge. Toggle in-session with `Ctrl+Shift+F`, the
  nav/edit `Focus` key, or `\focus`. The status bar shows `FOCUS` while on; the
  header and keyboard return on exit.
- **Word count** — the status bar shows `W n` (whitespace-delimited words across
  the whole document), refreshed as you type. Disable with
  `P4_CONFIG_EDITOR_WORD_COUNT=0`.
- **Spellcheck** — put a wordlist at `sd:/DICTS/<name>.words` (one lower-case
  word per line; default `<name>` is `en`; a curated sample ships in
  `apps/dicts/`, push it with `python apps/push_dicts.py <COM_PORT>`).
  Toggle with `Ctrl+Shift+S`, the `Spell` key, or `\spell`; misspellings are
  underlined and the status bar shows `SPELL`. With no list present the toggle
  tells you the expected path and stays off. Tokenization is UTF-8 aware
  (`don't`/`well-known` stay whole; CJK, digits, `_`, and non-ASCII Latin are
  never flagged), single-character words are checked like any other, and
  underlines compose with word-wrap.
- **Reading typography** — `view` (for `.md`) and the editor Markdown preview
  use the `reading` font role: a vendored `DejaVuSerif` face (auto-selected
  once pushed to `sd:/FONTS/`) with reader line spacing. `font set reading
  <name>` / `font size reading <px>` override it; the terminal role stays
  monospace.
- **Templates** — `edit DIARY.TXT /template NOTE` starts a **new** buffer
  pre-filled from `sd:/TEMPLATES/NOTE.MD` (an existing file is never
  overwritten). Ship the samples from `apps/templates/` with
  `python apps/push_templates.py <COM_PORT>`. The `WRITER` app demonstrates the
  flow.

## 16. Editor limits

| Value | Default |
|-------|---------|
| Maximum file size | `P4_CONFIG_EDITOR_MAX_BYTES` (1 MB) |
| Maximum lines | `P4_CONFIG_EDITOR_MAX_LINES` (65536) |
| Undo depth | `P4_CONFIG_EDITOR_UNDO_DEPTH` (64) |
| Undo snapshot byte budget | `P4_CONFIG_EDITOR_UNDO_MAX_BYTES` (4 MB) |
| Undo disabled above | `P4_CONFIG_EDITOR_UNDO_MAX_SNAPSHOT_BYTES` (256 KB) |
| Rendered rows at once | `P4_CONFIG_EDITOR_RENDER_ROWS` (256) |
| Tab width | `P4_CONFIG_EDITOR_TAB_WIDTH` (4) |
| Find / Replace string length | `P4_CONFIG_EDITOR_FIND_BYTES` (128) |
| Spell wordlist cap | `P4_CONFIG_SPELL_MAX_BYTES` / `_MAX_WORDS` (1 MB / 65536) |
| Template seed cap | `P4_CONFIG_TEMPLATE_MAX_BYTES` (8 KB) |
| Line-number gutter width | `P4_CONFIG_EDITOR_LINE_NUMBER_WIDTH_CHARS` (5) |

Files beyond the byte/line limits are refused with an error, never truncated.

The document is stored in **PSRAM**, and only a window of
`P4_CONFIG_EDITOR_RENDER_ROWS` rows is materialized on screen at once (the
window follows the cursor and the scroll position), so large files open and
edit responsively. Above the snapshot threshold the editor disables undo for
the session to avoid copying the whole document on every keystroke.

---

## 17. Edge cases and tips

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

## 18. Troubleshooting

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
- **`edit /template` warns instead of seeding** — the target already exists
  (templates only seed *new* files), the name is invalid (letters, digits,
  `_`/`-` only), or `sd:/TEMPLATES/<name>.MD` is missing (push with
  `python apps/push_templates.py <COM_PORT>`). An over-8 KB template is
  seeded truncated with a warning.
- **`Spell` stays off** — no `sd:/DICTS/<name>.words` on the card (push the
  `apps/dicts/` sample with `python apps/push_dicts.py <COM_PORT>`).
  Coverage is exactly the wordlist: add domain words (one per line) for
  false-flagged terms.
- **Preview reports `preview needs a Markdown file`** — preview renders
  Markdown only; rename to `.md`/`.markdown`/`.mkd` or open a Markdown file.
  **`too large to preview`** means the document is past the 96 KB preview
  cap — split it or share it with `markdown export` instead.
- **`markdown export` refuses the file** — sources past 64 KB are not
  exported, and output past the 256 KB export cap is not written; split the
  source or export a smaller range.
- **Serif preview falls back to a bitmap face** — `sd:/FONTS/` has no reading
  serif yet; push with `python push_fonts.py <COM_PORT>` and
  `font set reading <name> /save`.
- **The `WRITER` demo reports a failure** — run it as `WRITER` and read the
  `FAIL` line: it names the missing push (`push_templates.py`,
  `push_dicts.py`) or the failed export step.

---

## 19. Implementation notes

The editor lives in `components/editor/`:

- `editor.c` — the byte-preserving document model (lines, cursor, selection,
  undo/redo, find/replace, the pure line-number gutter formatter, SD
  load/save) and the worker-task session.
- `editor_view.c` — the LVGL surface: syntax-coloured spans with a
  line-number gutter, a current-line highlight, a blinking block cursor, a
  selection overlay, and the status-bar prompt system (Find/Replace/Go-to/
  Save-As/Open/quit confirm). The pure `editor_osk_key_from_label()` mapper is
  the single label→key table shared by the touch handler and the unit tests,
  and the view selects the OSK Nav page on open and the letters page for text
  prompts.
- `editor_view.h` — the view's public key/OSK/serial entry points
  (`editor_osk_key_from_label`, `editor_view_notify_opened`).

The editor reuses the transcript container as its surface
(`windows_enter_editor_mode`), keeps it visible at the transcript-region
height (so it is exactly the size of the shell transcript), hides the shell's
span group while open, and restores it on close. Since v0.33.0 the editor is a
surface on the shared modal runtime in `components/modal/`; input routes
through the generic `shell_command_ops_t.modal_*` hooks (`modal_is_active`,
`modal_handle_usb_key`, `modal_handle_serial_line`) — the same ops-table
pattern used by `dialog`, `list`, and `ask`.

> **Other modals:** `edit` is one of six `modal_surface_t` surfaces on the shared runtime (`dialog`, `list`, `ask`, `browse`/`filebrowser`, `view`, `hexview`). The TUI cell buffer (`components/tui/`), the `draw` verbs, and the `gfx` RGB565 canvas are separate surfaces reached through the `draw`/`gfx` batch verbs; all render into the live transcript region (80x25).
> See `documentation.md` (architecture), `SDK.md` (integration), and `command.md` (verbs). The Companion is 10 pushed BATs and hardware-verified (deep 8/8 on COM3).

All tunables are `P4_CONFIG_EDITOR_*` and documented in
`p4minishell_config.yaml`.
