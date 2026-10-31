# Machine-readable source comments

P4MiniShell keeps two kinds of knowledge about the code: **human prose** (the
`@brief`/description blocks every public header already carries) and
**machine-readable structure** (the tags documented here). The tags are plain
Doxygen tags, so they read as ordinary descriptive comments — there is no
`// AI:` prefix, no metadata block, and no second source of truth. A tool
extracts them into a manifest that agents, the breadcrumb map, and the release
gate all consume.

- Extractor / validator: [`tools/comment_index.py`](../tools/comment_index.py)
- Generated manifest: [`comment-index.json`](../comment-index.json)
- Breadcrumb map built on it: [`tools/breadcrumbs.py`](../tools/breadcrumbs.py)
  → [`breadcrumbs.json`](../breadcrumbs.json) + [`BREADCRUMBS.md`](../BREADCRUMBS.md)

---

## 1. The tag vocabulary

Every tag is a single line inside the leading `/** ... */` block of a component's
**primary header** (the header named after the component, e.g.
`components/shell/shell.h`). One tag per line, no wrapping.

| Tag | Argument | Meaning |
|-----|----------|---------|
| `@module` | component directory name | The owning component. Must equal the directory name under `components/`. |
| `@layer` | `main` \| `command` \| `batch` \| `storage` \| `shell` \| `leaf` \| `app` | The architectural layer (the one-way dependency chain `main → command → batch → storage → shell → leaves`). |
| `@domain` | topic keyword | The subsystem topic (`core`, `fs`, `ui`, `net`, `media`, `data`, `crypto`, `platform`, `app`). |
| `@provides` | free text | The capability this module provides, in one line. |
| `@status` | `stable` \| `experimental` \| `deprecated` | Lifecycle state. |
| `@test` | comma-separated repo-relative paths | Unit-test file(s) covering the module (Layer 1). Empty/absent is a warning, not an error. |
| `@hard-rule` | `doc.md#anchor` | The rule that constrains this module (a section of `ai-context.md`, `SDK.md`, ...). |

Example (`components/shell/shell.h`):

```c
/**
 * @file shell.h
 * @module shell
 * @layer shell
 * @domain core
 * @provides transcript, prompt, key queue, history, debug log, UART console bridge
 * @status stable
 * @test test/main/test_shell_parser.c, test/main/test_shell_history.c
 * @hard-rule ai-context.md#shell-core-rules
 * @brief Shell core module for P4MiniShell — transcript, history, debug log,
 *        UART console, boot banner, system info commands, and utility functions.
 * ...
 */
```

The order above (`@file`, then the machine tags, then `@brief`) is the house
style: the machine-readable identity is grouped at the top of the block, and
the human prose follows. Doxygen accepts any order; the extractor does not care.

---

## 2. Rules

- **One primary header per component carries the tags.** Secondary headers
  (`ansi_palette.h`, `header_layout.h`, ...) do not need them; the component is
  the unit of the manifest.
- **No duplication.** `@module`/`@layer`/`@domain`/`@provides` describe the
  component once. Do not repeat them per file, and do not restate them in
  prose.
- **`@requires` is not a tag.** The dependency set is already machine-readable
  in each component's `CMakeLists.txt` `REQUIRES` list. The extractor reads that
  as the single source of truth; never hand-copy it into a comment.
- **`@test` must point at real files.** The validator checks each path exists.
- **`@hard-rule` must resolve.** The validator checks the document exists and
  that the anchor is a real heading in it.
- **`@status` must be one of the three values.** Anything else is a validation
  error.
- **Never add `// AI:` comments.** The tags above are the sanctioned way to make
  a comment machine-readable; a `// AI:` marker is still a release-blocking
  defect (see `release.md`).

---

## 3. The manifest

`tools/comment_index.py` writes `comment-index.json`:

```json
{
  "version": 1,
  "generated_by": "tools/comment_index.py",
  "layers": ["main", "command", "batch", "storage", "shell", "leaf", "app"],
  "modules": {
    "shell": {
      "path": "components/shell",
      "header": "components/shell/shell.h",
      "layer": "shell",
      "domain": "core",
      "provides": "transcript, prompt, key queue, ...",
      "status": "stable",
      "tests": ["test/main/test_shell_parser.c"],
      "rules": ["ai-context.md#shell-core-rules"],
      "requires": ["ansi", "display", "windows", "header", "keyboard", "clock"]
    }
  }
}
```

`requires` comes from `CMakeLists.txt`; everything else comes from the tags.

---

## 4. Checks the validator performs

Run `python tools/comment_index.py --check` (or `--write` to regenerate the
manifest). It exits non-zero on any **error**:

- a component directory with no tagged primary header;
- `@module` that does not match its directory name;
- unknown `@layer` / `@domain` / `@status` value;
- `@test` path that does not exist;
- `@hard-rule` whose document or anchor does not exist;
- a `// AI:` comment anywhere under `components/`, `main/`, `boards/`, `test/`;
- a forbidden dependency edge (the inversions called out in
  `ai-context.md` §Module Layering Rules), e.g. `shell` requiring `command` or
  `batch`, `storage` requiring `batch`/`command`, `applib` requiring
  `command`/`batch`/`networking`, or any component requiring `main`;
- a forbidden header `#include` (e.g. `components/shell/**` including
  `command.h`).

**Warnings** (reported, do not fail the gate): a tagged component with no
`@test` line, or a public function in a tagged header with no `@brief`.

---

## 5. Regenerating

After adding or renaming a component, or after changing its tags:

```powershell
python tools/comment_index.py --write     # refresh comment-index.json
python tools/breadcrumbs.py --write       # refresh breadcrumbs.json + BREADCRUMBS.md
```

Both are committed generated artifacts (like `components/gfx/gfx_font.c`), so
review the diff. The release gate runs `--check` on both.
