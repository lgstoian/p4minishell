# Investigation report

**Question:** Why did P4MiniShell v1.4.3 ship 15 wrong docs/assets/apps PNGs and a verification harness that reported green, and does the redesigned harness now detect the exact defect class?

**Done when:** Findings recorded for the false-green mechanism, the observed wrong pixels, and the canary that proves the new guard can go red; plan complete; handoff written.

## What we looked at

- gallery_audit.py (A text file (notes, source code or settings))
- D:\Code\new\p4minishell\release\p4minishell-src.zip (shipped source)
- bad gallery PNG from git HEAD~1 (extracted to temp image)
- D:\Code\new\p4minishell\tools\gallery_audit.py and tools/suites/s25_apps_gallery.py (redesigned gates)

## What we found

### Observed in the shipped source (git HEAD~1, tools/suites/s25_apps_gallery.py): missing start/step/done markers and failed streaming captures were reported with c.note(), never c.check(), and the stream screenshot was taken after waiting for the app's done marker. The only publish predicate was size + ink + colour count, so a screenshot of the shell prompt satisfied every assertion. This is the false-green mechanism that shipped the wrong gallery PNGs.

- **Confidence:** Seen it (observed)
- **Evidence:**
  - `ev_51762e86e96996b15527dd83e67cb0a1320c9fd356d4706f7869eb4081c38e73` — shipped_s25.py: lines 471–515 of 688 (UTF-8). (path=C:\Users\lgstoian\AppData\Local\Temp\opencode\shipped_s25.py)

### The shipped agenda.png artifact is a distinct 6883-byte PNG with SHA-256 115368f6c76be0e58beae108d9fcd90559daf1797c2f9c87a6fa9760fa1b601f. The engine does not decode PNG, so pixel content was established by the host tool gallery_audit.py, which reported the shipped set as 13 shell-capture PNGs and 3 near-duplicate pairs.

- **Confidence:** Seen it (observed)
- **Evidence:**
  - `ev_5f5968c4e6efb3d10810bfced3b9add530fc190709bc47ecbaf0c85badae3d35` — shipped_agenda.png isn't a format we recognise yet.
  - `ev_f63cf388839b444cefd979ed06cbe911895c2e4ad043420331237158828cbd82` — gallery_audit.py: lines 1–60 of 181 (UTF-8). (path=D:\Code\new\p4minishell\tools\gallery_audit.py)

### The redesign adds an independent identity gate (shell-chrome strip + last-ink-row + near-duplicate signature), stages captures away from docs/, requires every app marker to be a check, and adds a self-test canary that synthesizes a shell dump and a duplicate and requires the guard to fail. The canary passed (bad=1, good=0), so the guard is demonstrably capable of going red.

- **Confidence:** Worked it out (derived)
- **Evidence:**
  - `ev_f63cf388839b444cefd979ed06cbe911895c2e4ad043420331237158828cbd82` — gallery_audit.py: lines 1–60 of 181 (UTF-8). (path=D:\Code\new\p4minishell\tools\gallery_audit.py)

## Open questions

_None outstanding._

## How this was done

- **Mode:** Deep (the original's code and files could be read)
- **Engines:** builtin
- **Limitations:**
  - The file is read as text as it is stored: nothing is run, rendered or formatted, and text a program builds or downloads while it runs isn't here.
  - the kind is detected from the first bytes and, to break ties, the file name

## Evidence ledger

- `ev_890ec96443a593a204f45f09652277a6ffe8b2bbcbb2439ff8aad3a529b059c1` **core_target_triage** — shipped_agenda.png is a text file (a script, notes or settings). (Best guess)
- `ev_5f5968c4e6efb3d10810bfced3b9add530fc190709bc47ecbaf0c85badae3d35` **core_target_triage** — shipped_agenda.png isn't a format we recognise yet. (Best guess)
- `ev_f63cf388839b444cefd979ed06cbe911895c2e4ad043420331237158828cbd82` **core_text_read** — gallery_audit.py: lines 1–60 of 181 (UTF-8). (Seen it)
- `ev_51762e86e96996b15527dd83e67cb0a1320c9fd356d4706f7869eb4081c38e73` **core_text_read** — shipped_s25.py: lines 471–515 of 688 (UTF-8). (Seen it)

