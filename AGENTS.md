# AGENTS.md - start here

This file is the orientation page for any AI agent (or new contributor) working
in P4MiniShell. Read it, then follow the pointers. It is the companion to
[`ai-context.md`](ai-context.md) (the working agreement) and
[`architecture.md`](architecture.md) (the system views).

---

## 1. The 60-second orientation

| I want to... | Read |
|--------------|------|
| Know the rules before touching code | [`ai-context.md`](ai-context.md) §0 Ground rules, §Hard Rules, §Module Layering Rules |
| See the system shape | [`architecture.md`](architecture.md) |
| Find a module and what governs it | [`BREADCRUMBS.md`](BREADCRUMBS.md) + `breadcrumbs.json` |
| Change a command | [`command.md`](command.md) |
| Change the batch language | [`batch.md`](batch.md) |
| Add/change a native app | [`ABI.md`](ABI.md), [`SDK.md`](SDK.md), [`docs/native_packaging.md`](docs/native_packaging.md) |
| Touch hardware (pins, buses, display, audio, power) | [`schematics.md`](schematics.md) first |
| Add a board | [`PORTING.md`](PORTING.md) |
| Understand the test ladder | [`harness.md`](harness.md), [`test/README.md`](test/README.md) |
| Understand a host tool | [`tools/README.md`](tools/README.md) |
| Report/track a bug | [`bugs.md`](bugs.md) |
| Cut a release | [`release.md`](release.md) |

**Layer order (dependencies flow one way):**
`main -> command -> batch -> storage -> shell -> leaf -> app`.
The only upward edges are inverted through registration tables; never add a
trampoline.

---

## 2. Machine-readable comments (breadcrumbs for agents)

Every component's primary header carries a compact, machine-readable tag block
(`@module`, `@layer`, `@domain`, `@provides`, `@status`, `@test`, `@hard-rule`).
It is plain Doxygen - no `// AI:` prefix - and it is the single source of truth
for the navigation map. The vocabulary and rules are in
[`docs/comment-tags.md`](docs/comment-tags.md).

```powershell
python tools/comment_index.py --write     # validate + refresh comment-index.json
python tools/breadcrumbs.py --write       # refresh breadcrumbs.json + BREADCRUMBS.md
python tools/breadcrumbs.py --for components/shell/shell.c   # where am I?
python tools/breadcrumbs.py --layer shell                     # what is in this layer?
```

When you add or rename a component, tag its primary header and regenerate both
artifacts in the same change.

---

## 3. Leave a trail (agent activity breadcrumbs)

An agent session should leave a short, machine-readable trail so the next agent
(or a human) can follow what was touched and why. Record it as you finish a
task:

```powershell
python tools/agent_trail.py record --task "add breadcrumb verb" `
    --files components/command/breadcrumb_commands.c `
    --summary "added the breadcrumb verb + pure formatter" `
    --breadcrumb components/command
python tools/agent_trail.py list          # recent entries
python tools/agent_trail.py summary       # grouped by module
python tools/agent_trail.py export-md     # write .agent/AGENT_TRAIL.md for the next agent
```

The trail is stored in `.agent/trail.jsonl` (git-ignored). `--files git` records
the currently modified files from `git status`.

---

## 4. Never ship a false green

A test that passes without asserting anything is a defect, not a pass. The
static guard scans the Unity suite and the host suites for vacuous tests,
declared-but-never-run tests, tautological assertions, and zero-check suites:

```powershell
python tools/green_guard.py            # scan; non-zero on any error
python tools/green_guard.py --strict   # warnings become errors
```

The runner also treats a zero-assertion `Checklist`/`SuiteResult` as a failure
at run time, so a vacuous suite can never report green. Run the guard before
committing test changes; `release.md` gates on it.

---

## 5. Definition of done

A change is finished only when: firmware **and** `test/` build with zero errors
and zero warnings, the on-board unit suite is green, the affected hardware
behaviour is verified, and the static guards pass. Then reflash the main
firmware. See [`ai-context.md`](ai-context.md) §Hard Rules and
[`harness.md`](harness.md) §0.

```powershell
python tools/green_guard.py
python tools/comment_index.py
python tools/breadcrumbs.py
python tools/harness/verify.py
```
