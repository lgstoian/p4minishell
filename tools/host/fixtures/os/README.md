# `os` facade host fixtures

`snapshot.json` holds mock accessor snapshots for the read-only `os` facade.
Each case supplies the argv the pure parser sees, the fields an owner accessor
would have produced, and the exact `key=value` line that must come out.

`tools/host_os_gate.py` extracts the pure parse/format declarations from
`components/command/command.h` and the implementation from
`components/command/os_commands.c`, compiles them with a generated C harness,
and checks the emitted lines against these baselines. Python re-implements no
formatter: the shipped C code runs natively with mock data.

The fixture deliberately includes `unavailable` cases (networking host ops and
battery ADC): a field whose accessor could not answer must render
`key=unavailable`, never a stale or invented value. The `pwr` cases cover the
read-only power/sleep/battery area; they describe *configured* wake sources,
never claimed-armed ones (arming happens only in the sleep verbs).

This is a hardware-free pre-gate only. It signs off nothing about a board
(`harness.md` §0/§2.2).
