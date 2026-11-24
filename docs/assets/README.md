# Public screenshots (`docs/assets/`)

Committed, curated screenshots for the public docs. This folder is the
**only** screenshot location that ships with the repository.

`proof-index.html` is the checked, board-free proof set for v1.5.0: it shows
only captures verified by visual inspection and the identity/colour gates, with
host-generated proofs labelled as such. It is not hardware sign-off.

- `screenshots/` (repo root) is git-ignored scratch from test runs — never
  link docs to it. It should stay empty (only `.gitkeep`) on a clean checkout.
- Everything in this folder is committed: keep it small. Store **PNG only**
  (no 1.8 MB BMPs), 1024x600 native panel size, descriptive kebab-case names.

## Live proof

`docs/assets/proof/` holds the required live-browser proof: a panel capture of
`https://en.wikipedia.org/wiki/ESP32` rendered over HTTPS on the device, the
matching `tui dump` transcript, and the same app screenshot published as
`apps/web.png`. `s21_wikipedia` stages these under `screenshots/proof/` and
`tools/publish_proof.py` is the only publish path. The live leg is mandatory:
a gate that skips it is a failure, not a pass.

## App captures

`<surface>-<subject>.png` — the name must say what the shot shows:

| File | What to capture |
|------|-----------------|
| `shell-idle.png` | Clean prompt after `cls`, header visible, on-screen keyboard shown |
| `shell-help.png` | `help` output with colour transcript |
| `shell-dir.png` | `dir` listing showing colour by kind (dir blue, `.bat` green) |
| `editor-editing.png` | `edit` open on a `.bat` with gutter + status bar + Nav page |
| `modal-dialog.png` | `dialog` modal centred over transcript |
| `modal-list.png` | `list` modal with themed selection highlight |
| `tui-boxes.png` | `draw box` single/double/rounded demo filling the transcript region |
| `gfx-bounce.png` | `BOUNCE` gfx canvas mid-animation |
| `plot-graph.png` | `plot func` axes + sine curve (canvas or TUI) |
| `apps-tcmd.png` | `TCMD` dual-pane commander |
| `apps-snake.png` | `SNAKE` gameplay |
| `theme-amber.png` / `theme-ice.png` / `theme-mono.png` | Same idle shell under each theme |
| `browser-website.png` | `WEB.BAT` showing the browser rendering `sd:/WEB/INDEX.HTML` (headings, links, list, table) |
| `header-status.png` | Close crop or full shell showing Wi-Fi/MEM/CPU/BAT header |
| `wifi-status.png` | `wifi status` connected output |
| `wiki-live.png` | Live `en.wikipedia.org/wiki/ESP32` (render action) fetched and rendered whole |
| `apps/<app>.png` | One main-screen capture per reference app (`s25_apps_gallery`); indexed in [`../apps.md`](../apps.md) |

Aim for ~12 shots minimum: shell, files, editor, one modal, TUI, gfx, plot,
one app, one theme variant, header, and the browser.

`docs/assets/apps/` holds the per-app gallery captures (22 PNGs, 1024x600,
`<app>.png`). They are produced by
`python tools/p4test_run.py <COMx> --only s25_apps_gallery` stages into `screenshots/gallery/`; `python tools/publish_gallery.py --yes` is the only publish path, and
`python tools/gen_apps_gallery.py` writes [`../apps.md`](../apps.md) from them. The browser shot is
reproduced by `python tools/p4test_run.py <COMx> --only s20_website`, which
renders `apps/website/INDEX.HTML` offline and leaves the BMP in
`screenshots/regression/browser-website.bmp`; convert it to
`docs/assets/browser-website.png` (PNG only).

## How to capture (build mode, board attached)

The firmware is remote-controllable over USB serial, including screenshot
capture. The curated set is produced by one script (non-destructive: no
persistence writes, editor quits without saving, theme restored to default):

```powershell
# Replace <COM_PORT> with your port, or set $env:P4_PORT
python tools/capture_docs.py <COM_PORT>
```

It drives each state over the shell, grabs the live screen with the
streaming `screenshot` protocol (`tools/p4test/screenshot.py`), and writes
PNG only to this folder. Single shots work the same way:

```powershell
python tools/harness/grab_screenshot.py --port <COM_PORT> --out screenshots
```

Notes:

- Run `cls` first for clean shell shots so scrollback does not offset modals.
- The bare streaming `screenshot` command also works while a modal or the
  `edit` editor is open (handled by the console-reader task).
- Convert BMP to PNG with Pillow before committing; never commit `.bmp`.
- Check each PNG opens at 1024x600 and shows the intended surface fully
  on-screen with no clipping.
- After capturing, reference the image from the docs, e.g. in `readme.md`:

```markdown
![Shell idle](docs/assets/shell-idle.png)
```

## Linking rule

- Public `.md` files link **only** to `docs/assets/*.png` (relative path).
- Never link to `screenshots/...` (ignored scratch) or `spikes/...`.
- Every image must have alt text describing what it shows.
