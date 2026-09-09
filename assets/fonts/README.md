# Bundled SD fonts (Phase 3)

Optional TrueType/OpenType fonts pushed to `sd:/FONTS/` by `push_fonts.py`.
Verify with `SHA256SUMS`. Never loaded at boot; selected live via
`font set <terminal|ui> <name>` (see `command.md`).

| File | Source | License | Notes |
|------|--------|---------|-------|
| DejaVuSansMono.ttf | dejavu-fonts 2.37 release | LICENSE-DejaVu.txt (Bitstream Vera + PD changes) | monospace; terminal-capable |
| NotoSans-Regular.ttf | googlefonts/noto-fonts (hinted) | OFL.txt | proportional; UI role only |
| NotoSansSC.ttf | google/fonts variable `[wght]` | OFL.txt | CJK display; default (Regular) instance used |

The built-in default (`unscii_16` + Montserrat fallback) is always compiled
in and never requires these files: a board without SD shows the full default
UI. Missing/corrupt TTFs fail `font set` with an errorlevel, never a bricked UI.
