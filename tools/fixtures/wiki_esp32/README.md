# wiki_esp32 - offline browser acceptance fixture

Source page: <https://en.wikipedia.org/wiki/ESP32>
Captured: 2026-11-16
Article revision data was read from the Wikipedia API on the capture date.

This is a **trimmed but structurally faithful** copy of the article for the
deterministic `s21_wikipedia` browser suite. It is not a full mirror:

- The lead paragraph, the infobox, the "Features", "QFN packaged chip and
  module" and "Programming" sections are kept, with a few paragraphs each.
- The large "ESP32 family" and module-vendor tables are reduced to a small
  feature table (the renderer caps a table at 8 rows / 5 columns).
- Every URL is rewritten to a local `sd:/WIKI/...` target, so the page renders
  with no AP, no DNS and no HTTP server. `PROGRAMMING.HTML` is the link target.
- `ESP32-MODULE.PNG` and `ESP32-DIE.JPG` are small generated placeholders,
  not the original Wikimedia photographs. They exercise the PNG and JPEG
  decoders (`components/imgdec`) and keep the fixture under 60 KB.
- `ESP32-DIE-MISSING.PNG` is intentionally absent. The renderer must fall back
  to the image's `alt` text ("ESP32 die shot unavailable") when a source is
  missing; the suite asserts that degradation.

Content licence: Wikipedia text is CC BY-SA 4.0; this trimmed excerpt keeps the
same licence and attribution. The generated placeholder images are original
fixtures under the repository's MIT licence.

Total fixture size: well under the 60 KB budget (about 11 KB).
