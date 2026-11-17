# Sample emoji pack (v1.4.3)

Project-authored artwork, MIT (see [`licence.md`](../../licence.md)). It is the
deterministic fixture for the emoji tests and a copyable example of the pack
format.

```
apps/emoji/EMOJI.INI    index
apps/emoji/GRIN.BMP     U+1F600
apps/emoji/HEART.BMP    U+2764
apps/emoji/STAR.BMP     U+2B50
```

Deploy it to SD (and optionally USB):

```powershell
python apps/push_apps.py <COM_PORT>              # writes sd:/EMOJI/
python apps/push_apps.py <COM_PORT> --drive usb: # writes usb:/EMOJI/
```

Format: `HEXCODEPOINT=FILE` (BMP or PNG relative to `EMOJI/`) and optional
`HEXCODEPOINT.cells=1|2` (TUI width, default 2). The firmware's built-in retro
map is always the fallback, so a missing or malformed pack never breaks text.

Verify on the device:

```
emoji            (source, count, sample)
emoji list       (every entry)
emoji check      (missing/oversized files)
emoji path 1F600 (resolved file)
```
