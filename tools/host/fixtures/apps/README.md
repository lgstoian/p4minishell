# Enriched-app host fixtures

One fixture per updated reference app in `apps/**`. Each fixture is a
**host-safe extract** of that app's `[M-RICH-*]` block: it exercises the same
additive batch-language surfaces (`str /rev|/count|/pad`, `map /set|/values`,
`for /K`, `func`/`return` publishing `%RESULT%`, and the wired
`trap /on errorlevel`) through the **real** interpreter ELF driven by
`tools/host_batch_run.py`. Nothing is reimplemented in Python.

The real apps also use hardware surfaces (gfx canvas, TUI, modal, webapp,
native loader). The host runner deliberately has no route for them, so they
are **not** put in these fixtures. `tools/host_apps_gate.py` finds those verbs
in the real app source and reports them as explicit SKIPs with the reason;
it never claims a host pass for them.

Run:

```powershell
python tools/host_apps_gate.py --check       # compare transcripts (default)
python tools/host_apps_gate.py --update      # regenerate baselines/*.txt
python tools/host_apps_gate.py --self-test   # red canaries, no ELF needed
```

A host pass is a pre-gate only: it proves the shared batch interpreter emits
the enriched markers stably. It says nothing about board rendering or modal
behaviour, which need the hardware suites (`harness.md`).
