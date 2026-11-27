# P4MiniShell browser: information and tutorial

The P4MiniShell browser is a small DOS-style web reader built into the shell.
It parses a bounded HTML subset, applies a CSS subset, runs in-page JavaScript
on QuickJS, and paints the result into the **same 80-column TUI cell grid the
shell uses**. Pages come from the SD card (offline) or from `http://` / `https://`
over the C6's hosted Wi-Fi. There is no second renderer, no second JS runtime and
no separate window manager: `browser`, `html` and the `webapp` facade all drive
the one DOM/CSS/canvas engine.

> **Proof labels in this document.** A **host proof** is generated on a PC from
> the real firmware render path: no board, no network, no hardware sign-off. A
> **live board proof** is a panel capture taken on a device over USB serial.
> The live capture linked below is an **archived v1.4.4 campaign capture**; it
> is not a new v1.6.0 board result. The v1.6 DOS-redesign live recapture is
> still pending (`harness.md` §0/§2.2).

## Wikipedia in our browser

**Host proof** — the saved ESP32 Wikipedia fixture rendered through the one
DOM/CSS/sampler into the DOS grid, first 25 rows only. It is not a live HTTPS
fetch and not the full page the device can hold in PSRAM:

![ESP32 Wikipedia article, first 25 rows, labelled host proof](docs/assets/web/wiki_esp32.png)

**Live board proof (archived v1.4.4 capture)** — the real
`https://en.wikipedia.org/wiki/ESP32` fetched over HTTPS on the device, with
the browser address bar, status header and on-screen input row visible. The
capture predates the v1.6 DOS-redesign markers; it is evidence of an earlier
hardware campaign, **not** v1.6.0 sign-off. The matching `tui dump` transcript
is [`wikipedia_live.txt`](docs/assets/proof/wikipedia_live.txt).

![Live on-device Wikipedia render from the v1.4.4 campaign](docs/assets/proof/wikipedia_live.png)

Both images show the same renderer; only their origin differs. The rest of this
page links the fixtures and tools that reproduce each one.

---

## 1. Opening a page

At the shell prompt:

```text
browser sd:/WEB/INDEX.HTML          # offline page from the SD card
browser https://en.wikipedia.org/wiki/ESP32
browser http://user:pass@host/path  # Basic auth in the URL
browser                             # print the verb summary
```

- Anything that is **not** `http://` or `https://` is treated as a local path
  and read through the guarded storage session (normally `sd:/...`). No network
  is needed.
- `http(s)` fetches go through the one HTTP client; TLS validation needs a
  valid wall clock (`sntp sync` or `date` first). A page larger than the
  2 MB fetch budget is refused with the real reason, not silently truncated.
- Protocol-relative URLs (`//upload.wikimedia.org/...`) are promoted to `https:`
  for both pages and images, which is what the live Wikipedia markup uses.
- `browser` keeps a 150-entry history and 32 bookmarks; both are persisted on
  SD (`sd:/BROWSER.HIST`, `sd:/BROWSER.BOOK`) and held in PSRAM at runtime.
  Back/forward is instant because a rendered-page snapshot is restored from
  the PSRAM page cache instead of being fetched and parsed again.

### `html` and `webapp` — the two facades

| Verb | What it does |
|------|--------------|
| `html <file\|url>` | Renders through the **same** browser engine as a blocking modal; `Esc`/`q` closes and restores the transcript. |
| `html /no <file\|url>` | Same render, non-blocking (the page stays up and the script continues). |
| `applib_html_open()` | Native-app entry point to the same page surface. |
| `webapp open [/no] <file\|url>` | Batch-facing wrapper over `html`. |
| `webapp dump [from] [to] [/a]` | Reads the rendered cell grid through `tui dump` (`/a` adds colour runs). |
| `webapp css` | Shows the computed CSS ruleset and winning cascade rule through `browser css`. |
| `webapp js <code>` | Evaluates JS through `js eval`. |
| `webapp event <id> <click\|load\|input\|change\|submit>` | Dispatches a DOM event to the already-rendered page; no second event registry. |
| `webapp tick` | Runs due JS timers and repaints; reports how many fired. |
| `webapp images` | Prints the last render's image outcomes (see below). |

The shipped batch shim is [`apps/webapp/WEBAPP.BAT`](apps/webapp/WEBAPP.BAT),
and the richer example page is [`apps/webapp/RICH.HTML`](apps/webapp/RICH.HTML).

### `browser` verb reference

| Verb | Effect |
|------|--------|
| `browser <url\|file>` | Fetch/render and push onto history. |
| `browser back` / `browser forward` | Move through history (page-cache hit when possible). |
| `browser swipe <back\|forward>` | Batch-callable version of the swipe gesture (left = forward, right = back). |
| `browser links` | List every tappable region with the command a tap would run. |
| `browser link <n>` | Resolve a relative `href` against the current page and follow it synchronously. `#fragment`, `javascript:` and `mailto:` targets are recognised and refused. |
| `browser js <element-id>` | Run an element's `onclick` handler and repaint. |
| `browser css` | Inspect rules, specificity, `!important` and order, plus computed `h1`/`th` colours. |
| `browser cache [clear]` | Report image/page cache usage, or clear both. |
| `browser bookmark [list]` / `add <name> <url>` / `del <name>` | Persisted bookmarks. |

---

## 2. `webapp images` outcome vocabulary

Each report line has this shape:

```text
webapp.image kind=img src=... state=resized native=800x600 fit=80x37 alt=...
webapp: N image outcome(s)
```

| Field | Values / meaning |
|-------|------------------|
| `kind` | `img` (element), `canvas` (bitmap drawn by JS), `background` (CSS `background-image` backdrop). |
| `src` | Resolved source URL, element id for canvas, or `-`. |
| `state` | `ok` (decoded at native cell size), `resized` (decoded then fitted to the page budget), `missing` (load/decode failed; alt text or no backdrop), `fallback` (no bitmap; element fallback content is shown). |
| `native` / `fit` | Native pixel size and the cell grid actually painted. |
| `alt` | Element alt text, or `-`. |

The report is transient page state, capped at 12 lines, and is reset on each
render. It comes from the same `dom_image_report_t` callback the renderer uses,
so there is no second image decoder or second vocabulary. A URL that fails to
decode is recorded as bad and is not refetched on later references.

---

## 3. What the page engine actually supports

### HTML subset

Headings, paragraphs, lists, links, images, tables (`colspan`, box-drawn
borders), `pre`, blockquotes, rules, semantic elements (`main`, `header`,
`nav`, `section`, `footer`, `figure`, `form` …), form inputs/select/button, and
`<canvas>`. `<script>` and `<style>` are treated as raw text, so a `<` inside
JS is not parsed as markup. Inline depth is capped at 24 levels and block
nesting at 32; past the cap the subtree degrades to flat text rather than
overflowing the command worker's stack. Entity decoding is longest-match with
legacy no-semicolon forms and numeric clamps; malformed/truncated markup is
repaired by the bounded parser before rendering. `<main>` gets reader focus so
mobile-site navigation cannot fill the viewport.

### CSS subset

Tag/class/id/attribute selectors, descendant and child combinators,
specificity (`id.class.type`), `!important` and source order; `color`,
`background`/`background-image`, alignment, font weight/decoration, opacity,
`rgba()`/`transparent`, units, gradients, shadows and rounded corners. The
cascade is one ruleset shared with the ANSI reader. A bounded responsive
`display:grid`-style column mapping is supported; **full flexbox/grid layout,
cookies, sessions and tabs are not supported by design**.

### JavaScript subset

One embedded QuickJS engine with `document.*` access, `addEventListener`,
`onload`/`onclick`, timers, `console.log`, `fetch` and DOM mutation from
scripts. Scripts and handlers run on the one timer pump; JS-created nodes are
rendered through the same DOM tree. The JS heap is capped at 512 KB and a page's
inline script budget is bounded.

### Canvas 2D

A pure RGBA Canvas 2D core implements fill/stroke, paths, arcs, transforms,
alpha, bitmap text, save/restore, gradients, shadows and rounded rectangles.
The canvas bitmap is snapshotted and mapped through the **same packed cell
image path as `<img>`**; a canvas with no 2D context shows its fallback content.

### DOS presentation

Every surface — shell, `.bat` app, browser page and native app — is drawn by
the one TUI cell renderer:

- 80 columns, 25 visible rows in landscape; a portrait panel turns the extra
  height into more rows while keeping 80 columns where a committed cell fits.
- CP437 box glyphs, a chunky bitmap cell font, the DOS/CGA 16-colour palette,
  an address bar at the top and a status line at the bottom.
- Images and canvas frames are nearest-neighbour sampled to DOS colour cells
  through the one RGB565 → luminance ramp → CGA palette path.
- Colours, `opacity` and `rgba()` are composited once into the DOS cell colour
  so the TUI and ANSI output agree.

---

## 4. Image support (verified from source)

| Format | Ingest path | Decoder | Notes |
|--------|-------------|---------|-------|
| BMP (24/32-bit `BI_RGB`, bottom-up or top-down) | `gfx_bmp_parse_header_ex()` → `gfx_bmp_decode_scaled_565()` in `components/gfx/` | project BMP core | The strict sprite form `gfx load` accepts 24-bit bottom-up. RLE and other bit depths are rejected. |
| PNG | `components/imgdec/` (`imgdec_probe` → `imgdec_decode_scaled`) | LodePNG (the copy LVGL already compiles) | Native dimensions must be within `P4_CONFIG_IMGDEC_MAX_W/H` (512×512) or the image is reported as unknown and falls back to alt text. |
| JPEG | `components/imgdec/` | TJpgDec (the copy LVGL already compiles) | Same native-size cap and fallback behaviour. |
| **WebP** | none | **not supported** | There is no WebP branch in `imgdec_probe()`, no WebP decoder in `components/`, and no WebP string anywhere in the firmware sources. An unsupported source shows its `alt` text. |
| GIF / SVG / AVIF and other web formats | none | not supported | Same honest fallback: alt text / fallback content. |

`components/imagefile/imagefile.c` is the one in-memory decoder: it routes
PNG/JPEG to `imgdec` and BMP to `gfx`, then returns an RGB565 surface.
`browser_load_image()` in `components/command/browser_commands.c` uses it for
both `<img>` and CSS backgrounds, samples the surface into DOS cells and caches
the result by resolved URL. Undecodable URLs are cached as bad so a page with
dozens of references to the same broken image does not refetch it.

**Direct answers:** JPG/JPEG **is** supported. WebP **is not** supported.
WebP is **not needed for the documented Wikipedia proof**: the offline
`wiki_esp32.html` fixture uses BMP images, and the live capture proves the
device renders the real article, whose image URLs are resolved through the
same protocol-relative `upload.wikimedia.org` handling. Format is sniffed from
magic bytes (not the file extension), so PNG/JPEG sources render whether the
URL ends in `.png`, `.jpg` or `.jpeg`. If a site serves only WebP, the image is
not decoded and the page shows its alt text; no WebP decoder is planned for
this firmware.

Licensing note: `imgdec` is a thin wrapper over the PNG/JPEG libraries already
shipped in vendored LVGL; it adds no new third-party source. See
[`licence.md`](licence.md).

---

## 5. Limits and honest degradation

| Limit | Value / behaviour |
|-------|-------------------|
| HTTP page body / SD page file | 2 MB (`P4_CONFIG_HTTP_MAX_BODY_BYTES`); oversized inputs are refused, not truncated. |
| Page buffer | 2000 rows in PSRAM (`P4_CONFIG_TUI_PAGE_ROWS`); 25 visible in landscape, more in portrait. |
| Tables | 8 rows × 5 columns, with `colspan` clamped to the remaining columns (`TABLE_MAX_ROWS` / `TABLE_MAX_COLS` in `dom_render.c`). |
| Image source | 64 KB per `<img>`/background (`P4_CONFIG_BROWSER_IMAGE_MAX_BYTES`); native PNG/JPEG dimensions capped at 512×512. |
| Image cell fit | 80×40 cells (`P4_CONFIG_BROWSER_IMAGE_MAX_WIDTH/HEIGHT`); all supported images render, scaled to fit, with no size filter. |
| URL / bookmarks / history | 512-byte URLs; 32 bookmarks; 150 history entries; image reports capped at 12. |
| JS | 512 KB heap, bounded inline script budget, one timer pump. |
| DOM | Node and arena caps in PSRAM; unbounded pages degrade rather than exhausting memory. |
| History/bookmark persistence | `sd:/BROWSER.HIST`, `sd:/BROWSER.BOOK`. |
| Network | Fetch is synchronous on the shell worker by design; the LVGL task keeps the display alive, but shell commands pause until the page is ready. |

---

## 6. Navigation: touch, mouse, keyboard and synthetic `ui`

- **Touch:** tap a link to follow it (the same resolver `browser link <n>`
  uses); drag to scroll the page; long-press then drag to select a cell range;
  tap inside the selection to copy it; swipe right/left for back/forward.
- **Mouse:** wheel scrolls the page; USB/BLE mice can drive an opt-in pointer
  (`usb mouse cursor|click on`), with `usb mouse move <x> <y>` and
  `usb mouse click` as the serial equivalents.
- **Keyboard:** arrow keys move/cursor; `PageUp`/`PageDown` scroll one visible
  page through the same viewport/present path as Up/Down; `Esc`/`q` closes the
  blocking `html` modal. On-screen keyboard keys auto-hide for a physical
  keyboard.
- **Selection:** one shared selection core backs the page, modal pagers and
  transcript scrollback. `ui selection copy|clear` is the script form.
- **Synthetic `ui` verbs** (batch-friendly; set ERRORLEVEL; `/b` bare output,
  `/v:NAME` result) let a script or host driver reproduce gestures without a
  finger: `ui tap <x> <y> [ms]`, `ui longpress <x> <y> [ms]`,
  `ui swipe <x1> <y1> <x2> <y2> [ms] [steps]`, `ui press <x> <y>`,
  `ui move <x> <y>`, `ui release`, `ui target <id> [ms]`,
  `ui key <label> [ms]`, `ui targets [/b] [/v:NAME]`,
  `ui hit <x> <y> [/v:NAME]`, `ui state [/b] [/v:NAME]`. While a modal blocks
  the worker, the console-reader path accepts these same gestures over serial.

---

## 7. Reproduce the proofs

### Offline Wikipedia host proof (no board)

Linux/WSL2 with ESP-IDF v5.5.5 sourced. These commands open no serial port and
are **pre-gates only**:

```powershell
python tools/host_browser_gate.py --check      # real DOM renderer on 5 fixtures, 271 assertions
python tools/host_browser_gate.py --self-test  # 9 red canaries; proves the gate can fail
python tools/host_webapp_gate.py --check       # text/colour-run baselines
python tools/make_webapp_proof.py --update     # DOS feature + form proofs (--check / --self-test too)
python tools/visual_proof_gate.py --check      # palette, CP437 borders, ink bands, signatures
python tools/make_web_screenshots.py --check   # committed DOS web proof PNGs
```

The fixture is [`wiki_esp32.html`](tools/host/fixtures/webapp/wiki_esp32.html);
the rendered proof is [`docs/assets/web/wiki_esp32.png`](docs/assets/web/wiki_esp32.png)
(first 25 rows only). The four DOS showcase pages are
[`portal.html`](tools/host/fixtures/webapp/portal.html),
[`dashboard.html`](tools/host/fixtures/webapp/dashboard.html),
[`article.html`](tools/host/fixtures/webapp/article.html) and
[`form.html`](tools/host/fixtures/webapp/form.html), rendered to
`screenshots/web/` with byte-identical documentation copies under
`docs/assets/web/`. See [`harness.md`](harness.md) §2.2 and
[`tools/README.md`](tools/README.md).

### Live board proof (hardware, when a board is attached)

`tools/p4test_run.py <COM_PORT> --only s21_wikipedia` with
`P4_WIFI_LIVE=1` fetches the real HTTPS article, requires a full render (a
size-cap degradation is not accepted), and stages
`screenshots/proof/wikipedia_live.png`, `web.png` and `wikipedia_live.txt`.
A skipped live leg **fails** the suite. `tools/publish_proof.py` is the only
publish path into `docs/assets/proof/`. The committed
[`docs/assets/proof/wikipedia_live.png`](docs/assets/proof/wikipedia_live.png)
is the archived v1.4.4 result; a v1.6.0 recapture is pending. See
[`harness.md`](harness.md) "Live-browser proof (hard gate)" and
[`docs/assets/README.md`](docs/assets/README.md).

---

## 8. Where to go next

- [`readme.md`](readme.md) — overview, quick start and the screenshot gallery.
- [`docs/apps.md`](docs/apps.md) — reference app gallery with proof origins.
- [`docs/assets/README.md`](docs/assets/README.md) — capture and publish rules.
- [`command.md`](command.md) — full command reference (`browser`, `html`, `webapp`, `ui`).
- [`documentation.md`](documentation.md) — module map, rendering and the one-renderer ADRs.
- [`ai-context.md`](ai-context.md) — image support rules and change discipline.
- [`harness.md`](harness.md) — test ladder and why a host pass is never hardware sign-off.
