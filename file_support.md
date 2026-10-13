# P4MiniShell File Format Support

Complete reference of all file formats supported by P4MiniShell, including viewing, editing, rendering, and processing capabilities.

---

## Document Formats

| Extension | Format | View | Edit | Render | Notes |
|-----------|--------|------|------|--------|-------|
| `.txt` | Plain Text | ✅ | ✅ | ✅ | Full syntax highlighting in editor |
| `.md` / `.markdown` | Markdown | ✅ | ✅ | ✅ | Full CommonMark + extensions (tables, fenced code, task lists) |
| `.html` / `.htm` | HTML | ✅ | ✅ | ✅ | ANSI-rendered in transcript, syntax highlighted in editor |
| `.epub` | EPUB e-book | ✅ | ❌ | ✅ | ZIP-based, OPF/NCX/nav parsing, chapter navigation |
| `.json` | JSON | ✅ | ✅ | ✅ | Validation, pretty-print, query via `json` command |
| `.csv` | CSV | ✅ | ✅ | ✅ | Grid view, `=EXPR` evaluation, `csv` command verbs |
| `.ini` | INI | ✅ | ✅ | ✅ | Section/key editing, `ini` command verbs |
| `.xml` | XML | ✅ | ✅ | ⚠️ | View/edit only, no rendering |
| `.yaml` / `.yml` | YAML | ✅ | ✅ | ⚠️ | View/edit only, no rendering |

---

## Image Formats

| Extension | Format | View | Edit | Render | Notes |
|-----------|--------|------|------|--------|-------|
| `.bmp` | BMP | ✅ | ❌ | ✅ | 24/32-bit BI_RGB, top-down/bottom-up, fit-to-screen decode |
| `.png` | PNG | ⚠️ | ❌ | ⚠️ | Via external tools only |
| `.jpg` / `.jpeg` | JPEG | ⚠️ | ❌ | ⚠️ | Via external tools only |
| `.gif` | GIF | ⚠️ | ❌ | ⚠️ | Via external tools only |

**Legend:** ✅ = Full support, ⚠️ = Partial/External, ❌ = Not supported

---

## Archive Formats

| Extension | Format | Create | Extract | List | Verify | Notes |
|-----------|--------|--------|---------|------|--------|-------|
| `.p4a` | USTAR (POSIX tar) | ✅ | ✅ | ✅ | ✅ | CRC manifest trailer, host tar compatible |
| `.zip` | ZIP | ⚠️ | ✅ | ✅ | ✅ | Stored (0) + Deflate (8), CRC-32 per member |
| `.epub` | EPUB (ZIP-based) | ❌ | ✅ | ✅ | ✅ | OPF/NCX/nav parsing, chapter rendering |
| `.tar` | USTAR tar | ⚠️ | ✅ | ✅ | ⚠️ | Via archive command if .p4a |

**Operations:**
- `archive create <file> <paths...>` — Create archive (auto-detect format by extension)
- `archive extract <file> [dest]` — Extract with safety checks
- `archive list <file> [/b]` — List contents (bare names with `/b`)
- `archive verify <file>` — CRC/checksum verification
- `backup <file> [paths...]` — System backup (USTAR only, defaults to DBS + alarms)

---

## Code & Script Formats

| Extension | Format | View | Edit | Run | Syntax Highlight |
|-----------|--------|------|------|-----|------------------|
| `.bat` | Batch | ✅ | ✅ | ✅ | ✅ |
| `.py` | Python | ✅ | ✅ | ❌ | ✅ |
| `.c` / `.h` | C/C++ | ✅ | ✅ | ❌ | ✅ |
| `.cpp` / `.hpp` | C++ | ✅ | ✅ | ❌ | ✅ |
| `.js` | JavaScript | ✅ | ✅ | ❌ | ✅ |
| `.json` | JSON | ✅ | ✅ | ❌ | ✅ |
| `.lua` | Lua | ✅ | ✅ | ❌ | ✅ |
| `.sh` | Shell | ✅ | ✅ | ❌ | ✅ |
| `.md` | Markdown | ✅ | ✅ | ❌ | ✅ |
| `.html` | HTML | ✅ | ✅ | ❌ | ✅ |
| `.css` | CSS | ✅ | ✅ | ❌ | ✅ |
| `.ini` | INI | ✅ | ✅ | ❌ | ✅ |

**Batch execution:** `launch <name>`, `start ( ... )`, `for`/`while`/`if`/`switch` control flow

---

## Data & Config Formats

| Extension | Format | Purpose | Tools |
|-----------|--------|---------|-------|
| `.ini` | INI config | App/device config | `ini` command, `appconfig` |
| `.csv` | CSV data | Tabular data | `csv` grid/eval, `db` import/export |
| `.json` | JSON | Data interchange | `json validate/pretty`, `db` import/export |
| `.vcard` / `.vcf` | vCard | Contacts | `pim` sync |
| `.ics` / `.ical` | iCalendar | Calendar events | `pim` sync, `alarm` |
| `.pem` / `.der` | X.509 certs | TLS trust store | `certs` command |
| `.p4a` | USTAR archive | Backups | `archive`, `backup` |
| `.zip` | ZIP archive | General archives | `archive` |
| `.rss` / `.atom` / `.xml` (feed) | RSS 2.0 / Atom 1.0 | Syndication feeds | `components/rss` (`rss_parse`, `rss_fetch`) |
| `.json` (feed) | JSON Feed 1.x | Syndication feeds | `components/rss` (parsed through `components/json`) |

---

## Executable & Firmware Formats

| Extension | Format | Purpose | Tools |
|-----------|--------|---------|-------|
| `.bin` | Binary firmware | OTA updates | `c6ota`, `send`/`receive` |
| `.elf` | ELF executable | Debugging | `addr2line` via host |
| `.p4a` | P4MiniShell archive | Backup/restore | `archive`, `backup` |

---

## Font Formats

| Extension | Format | Support | Notes |
|-----------|--------|---------|-------|
| `.ttf` | TrueType | ✅ | SD card fonts, `font set reading <name>` |
| `.otf` | OpenType | ⚠️ | May work if TTF-compatible |
| `.bdf` | Bitmap | ⚠️ | Limited support |
| `.fnt` | Custom | ✅ | Built-in `unscii_16`, `montserrat_14` |

---

## Audio Formats

| Extension | Format | Play | Record | Notes |
|-----------|--------|------|--------|-------|
| `.wav` | WAV (PCM) | ✅ | ✅ | `wavplay`, `mic` |
| `.mp3` | MP3 | ❌ | ❌ | Not supported |
| `.ogg` | Ogg Vorbis | ❌ | ❌ | Not supported |

---

## Special Formats

| Extension | Format | Purpose | Support |
|-----------|--------|---------|---------|
| `.p4a` | USTAR backup | System/user backups | Full |
| `.epub` | EPUB e-book | Reading | Read + render |
| `.zip` | ZIP archive | General archiving | Extract/verify/list |
| `.p4a` | USTAR | System backups | Create/extract/verify |

---

## Command Summary by Format

| Format | View | Edit | Render | Extract | Create | Verify |
|--------|------|------|--------|---------|--------|--------|
| Text (`.txt`) | `type`, `more` | `edit` | — | — | `write`, `append` | — |
| Markdown (`.md`) | `type`, `view` | `edit` | `markdown export` | — | `edit` | — |
| HTML (`.html`) | `type`, `view` | `edit` | `type` (auto) | — | `edit` | — |
| Markdown (`.md`) | `type`, `view` | `edit` | `markdown export` | — | `edit` | — |
| EPUB (`.epub`) | `epub read` | — | `epub read` | `epub extract` | — | `epub info` |
| ZIP (`.zip`) | `archive list` | — | — | `archive extract` | `archive create` | `archive verify` |
| USTAR (`.p4a`) | `archive list` | — | — | `archive extract` | `archive create`, `backup` | `archive verify` |
| Markdown (`.md`) | `type`, `view` | `edit` | `markdown export` | — | `edit` | — |
| JSON (`.json`) | `type`, `json pretty` | `edit` | `json pretty` | — | `write` | `json validate` |
| CSV (`.csv`) | `type`, `csv rows` | `edit` | `csv eval` | — | `write` | — |
| BMP (`.bmp`) | `view` | — | `view` (fit) | — | `gfx image` | — |
| JSON (`.json`) | `type`, `json pretty` | `edit` | `json pretty` | — | `write` | `json validate` |

---

## Rendering Pipeline

```
File → Detect format → Select renderer → Output
         ↓
    ┌────┴────┐
    │         │
 Text/Markdown  HTML/EPUB    Image     Archive
    │            │            │          │
 markdown.c  html_read.c    gfx/        archive.c
  (renderer)  (renderer)   imagefmt.c  (USTAR/ZIP)
    │            │            │          │
    └────┬─────┴────┬───────┴────┬─────┘
         │          │            │
         ▼          ▼            ▼
    ANSI SGR    ANSI SGR    RGB565      File list
    sequences   sequences   canvas    + metadata
         │          │            │          │
         └──────────┴────────────┴──────────┘
                    │
                    ▼
            Transcript / Display
```

---

## Configuration

All format limits are configurable via `p4minishell_config.h` / `p4minishell_config.yaml`:

| Config | Default | Description |
|--------|---------|-------------|
| `P4_CONFIG_HTML_RENDER_MAX_BYTES` | 256 KiB | Max HTML render size |
| `P4_CONFIG_MD_EXPORT_MAX_BYTES` | 256 KiB | Max Markdown export |
| `P4_CONFIG_IMAGE_MAX_BYTES` | 4 MiB | Max BMP image size |
| `P4_CONFIG_ARCHIVE_MAX_ENTRIES` | 512 | Max archive members |
| `P4_CONFIG_ARCHIVE_CHUNK_BYTES` | 4 KiB | Archive I/O chunk size |
| `P4_CONFIG_CHAT_INBOX_MAX` | 32 | Chat inbox message cap |
| `P4_CONFIG_CHAT_LOG_MAX` | 128 | Chat log message cap |

---

## Adding New Formats

To add a new file format:

1. **Detector:** Add to `filetype.c` `filetype_of()` with extension + magic bytes
2. **Renderer:** Implement in appropriate component (`markdown`, `html`, `gfx`, `archive`, `epub`)
3. **Command verbs:** Add to appropriate `*_commands.c` with help/completion
4. **Documentation:** Update this file (`file_support.md`) and `command.md`
5. **Tests:** Add unit tests in `test/main/test_<format>.c`
6. **Config:** Add `P4_CONFIG_*_MAX_BYTES` etc. to config header/YAML

---

## Version History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | 2026-10-12 | Initial: Text, Markdown, HTML, BMP, USTAR, ZIP, EPUB |
| — | — | — |