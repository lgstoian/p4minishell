# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Power / audio / LED / peripheral suite.

Every verb here is safe to run on a live board: audio is background playback,
RGB state is captured first and restored at the end, and ``gpio``/``pwm`` are
only *read*. ``sleep`` and ``deepsleep`` were deliberately left out of the
command path (they blank the panel and tear down Wi-Fi); only their usage/help
text is verified. The status LED camera (``tools/led_watch.py``) is not invoked
- that needs a webcam and belongs to the LED driver's own tool.
"""
from __future__ import annotations

import math
import os
import re
import struct

from p4test.asserts import Checklist
from p4test.session import PanicError

NAME = "power-audio"
TAGS = ["device"]
OUT_DIR = os.path.join("screenshots", "regression")


def _run(dev, c, label, cmd, timeout=30.0, settle=0.5):
    try:
        return dev.run(cmd, timeout=timeout, settle=settle)
    except PanicError:
        raise
    except Exception as exc:  # noqa: BLE001
        c.check(label, False, "%r did not return: %s" % (cmd, exc))
        return ""


def _restore_rgb(dev, status):
    """Put the WS2812 back the way we found it."""
    try:
        if "auto status" in status:
            dev.run("rgb auto on", timeout=15)
        else:
            m = re.search(r"#([0-9A-Fa-f]{6})", status)
            if m:
                dev.run("rgb #%s" % m.group(1), timeout=15)
            else:
                dev.run("rgb off", timeout=15)
    except PanicError:
        raise
    except Exception:  # noqa: BLE001
        pass


def run(dev, ctx):
    c = Checklist(NAME)
    quick = bool(ctx.get("quick"))
    disp_w, disp_h = dev.display_size()
    rgb_ok = dev.rgb_available()

    # -- audio -----------------------------------------------------------------
    out = _run(dev, c, "audio status", "audio status", timeout=15)
    c.expect("audio status field", "audio:", out)
    c.check("audio reports playing|idle",
            ("playing" in out) or ("idle" in out), out[-300:])

    out = _run(dev, c, "volume query", "volume", timeout=15)
    c.expect("volume prints a level", "volume:", out)
    m = re.search(r"volume:\s*(\d+)%", out)
    original_volume = int(m.group(1)) if m else None

    # Output routing: query the mode, force the speaker, round-trip /b + /v:,
    # then restore. The effective route is board-dependent (Tab5 follows the
    # headphone jack; the reference board is always the speaker).
    out = _run(dev, c, "output query", "audio output", timeout=15)
    c.expect("output prints mode", "audio.output:", out)
    c.expect("output prints route", "audio.route:", out)
    mo = re.search(r"audio\.output:\s*(\w+)", out)
    original_mode = mo.group(1) if mo else "auto"

    out = _run(dev, c, "output bare", "audio output /b", timeout=15)
    c.check("output /b is a bare route line",
            re.search(r"(?:^|\s)(speaker|headphones)\r?$", out, re.M) is not None,
            out[-120:])
    out = _run(dev, c, "output var", "audio output /v:P4ROUTE", timeout=15)
    out = _run(dev, c, "output var readback", "echo ROUTE=%P4ROUTE%", timeout=15)
    c.expect("output /v captures route", "ROUTE=", out)
    dev.run("set P4ROUTE=", timeout=15)

    out = _run(dev, c, "output set", "audio output speaker", timeout=15)
    c.expect("output set confirms", "audio.output:", out)
    out = _run(dev, c, "output set route", "audio output /b", timeout=15)
    c.check("forced speaker routes to speaker",
            re.search(r"(?:^|\s)speaker\r?$", out, re.M) is not None, out[-120:])
    out = _run(dev, c, "output restore", "audio output %s" % original_mode,
               timeout=15)
    c.expect("output restore confirms", "audio.output:", out)

    # Jack state is reported but not asserted: no plug is inserted during the
    # suite, so both "out" (Tab5, jack present in hardware) and "n/a"
    # (reference board, no jack) are correct.
    out = _run(dev, c, "output jack", "audio status", timeout=15)
    c.expect("status reports jack", "audio.jack:", out)
    m = re.search(r"audio\.jack:\s*(\S+)", out)
    c.note("jack state: %s" % (m.group(1) if m else "?"))

    # CONFIG.SYS persistence round-trips the mode through the key table.
    out = _run(dev, c, "config output set", "config AUDIO_OUTPUT headphones",
               timeout=15)
    c.expect("config saves output", "saved to", out)
    out = _run(dev, c, "config output sticks", "audio output", timeout=15)
    c.expect("config persisted headphones", "headphones", out)
    out = _run(dev, c, "config output restore",
               "config AUDIO_OUTPUT %s" % original_mode.upper(), timeout=15)
    c.expect("config restore saves", "saved to", out)

    out = _run(dev, c, "volume set", "volume 50", timeout=15)
    c.expect("volume set confirms", "volume set to", out)

    out = _run(dev, c, "beep", "beep", timeout=15)
    c.expect("beep returns immediately", "beep:", out)

    out = _run(dev, c, "tone", "tone 440 80", timeout=15)
    c.expect("tone returns immediately", "tone:", out)
    # Audio is background playback; let it drain before the next check.
    _run(dev, c, "audio stop", "audio stop", timeout=15)

    # wavplay only if the SD card already carries a WAV (none is deployed).
    listing = _run(dev, c, "dir for wav", "dir /b", timeout=25)
    wavs = [ln.strip() for ln in listing.splitlines()
            if ln.strip().upper().endswith(".WAV")]
    if wavs:
        out = _run(dev, c, "wavplay", "wavplay %s" % wavs[0], timeout=30)
        c.expect("wavplay starts", "wavplay:", out)
    else:
        c.note("wavplay skipped - no .WAV on the SD card")

    # Push a generated 22050 Hz mono 16-bit PCM sine so wavplay always runs:
    # no WAV fixture is deployed, so the suite brings its own and deletes it.
    wav_name = "P4S12T.WAV"
    wav_rate = 22050
    wav_n = wav_rate // 4
    wav_pcm = b"".join(
        struct.pack("<h", int(12000 * math.sin(2.0 * math.pi * 440.0 * i / wav_rate)))
        for i in range(wav_n))
    wav_bytes = (struct.pack("<4sI4s4sIHHIIHH4sI", b"RIFF", 36 + len(wav_pcm),
                             b"WAVE", b"fmt ", 16, 1, 1, wav_rate,
                             wav_rate * 2, 2, 16, b"data", len(wav_pcm))
                 + wav_pcm)
    try:
        dev.push_file(wav_name, wav_bytes)
        out = _run(dev, c, "wavplay fixture", "wavplay %s" % wav_name, timeout=30)
        c.expect("wavplay fixture starts", "wavplay:", out)
        _run(dev, c, "wavplay stop", "audio stop", timeout=15)
        _run(dev, c, "wavplay delete", "del /p %s" % wav_name, timeout=15)
    except Exception as exc:  # noqa: BLE001
        c.check("wavplay fixture", False, "push/play failed: %s" % exc)

    if original_volume is not None:
        _run(dev, c, "volume restore", "volume %d" % original_volume, timeout=15)

    # -- RGB status LED --------------------------------------------------------
    # Boards without a controllable LED (the Tab5 exposes one only when its
    # keyboard is attached) honestly refuse the verb; skip rather than fail.
    status = ""
    if rgb_ok:
        status = _run(dev, c, "rgb status", "rgb status", timeout=15)
        c.expect("rgb status title", "RGB LED", status)
        c.expect("rgb status driver field", "driver:", status)

        out = _run(dev, c, "rgb set", "rgb 255 0 0", timeout=15)
        c.expect("rgb colour set", "colour set to", out)
        out = _run(dev, c, "rgb off", "rgb off", timeout=15)
        c.expect("rgb off", "off", out)
        _restore_rgb(dev, status)
    else:
        c.note("rgb skipped - no controllable status LED on this board")

    # -- power / battery -------------------------------------------------------
    out = _run(dev, c, "power status", "power", timeout=20)
    c.expect("power pm field", "power.pm:", out)
    c.expect("power display field", "power.display:", out)
    c.expect("power idle field", "power.idle_off:", out)
    c.expect("power wake field", "power.wake:", out)

    out = _run(dev, c, "battery", "battery", timeout=20)
    c.expect("battery level", "battery:", out)
    # INA226 fuel-gauge boards print battery.gauge:; ADC-only boards print
    # battery.detail:.  Accept either so the suite is board-agnostic.
    has_detail = "battery.detail:" in out
    has_gauge = "battery.gauge:" in out
    c.check("battery detail or gauge", has_detail or has_gauge,
            "battery.detail: %s, battery.gauge: %s" % (has_detail, has_gauge))

    # sleep/deepsleep must NOT be executed here (display blank + Wi-Fi teardown);
    # only their documented usage strings are verified.
    out = _run(dev, c, "help sleep", "help sleep", timeout=15)
    c.expect("sleep usage text", "enter light sleep", out)
    out = _run(dev, c, "help deepsleep", "help deepsleep", timeout=15)
    c.expect("deepsleep usage text", "enter ESP deep sleep", out)

    # -- timer / stopwatch -----------------------------------------------------
    out = _run(dev, c, "timer start", "timer start p4test", timeout=15)
    c.expect("timer start", "timer: 'p4test' started", out)
    _run(dev, c, "timer delay", "delay 300", timeout=15)
    out = _run(dev, c, "timer lap", "timer lap p4test /v:P4LAP", timeout=15)
    c.expect("timer lap", "timer: 'p4test' lap at", out)
    out = _run(dev, c, "timer stop", "timer stop p4test /v:P4TOT", timeout=15)
    c.expect("timer stop", "timer: 'p4test' stopped at", out)
    out = _run(dev, c, "timer status", "timer status", timeout=15)
    c.expect("timer status heading", "Stopwatch", out)

    # -- peripherals (read-only) ----------------------------------------------
    out = _run(dev, c, "gpio status", "gpio status", timeout=20)
    c.expect("gpio status rows", "gpio.status:", out)
    out = _run(dev, c, "gpio list", "gpio list", timeout=20)
    c.expect("gpio list rows", "gpio.list:", out)
    out = _run(dev, c, "pwm status", "pwm status", timeout=20)
    c.expect("pwm status field", "pwm.status:", out)
    out = _run(dev, c, "adc status", "adc status", timeout=25)
    c.expect("adc status field", "adc.status:", out)
    out = _run(dev, c, "i2c scan", "i2c scan", timeout=30)
    c.expect("i2c scan field", "i2c.scan:", out)

    # SPI master transactions must work while the C6 hosted SDIO link is active
    # (SPI3 + GPIO-matrix pins; the SDIO link is on SDMMC, not a SPI host). This
    # guards the regression that once made `spi` refuse transactions.
    out = _run(dev, c, "spi status", "spi status", timeout=20)
    c.expect("spi status reports available", "spi.transactions:", out)
    c.expect("spi transactions available", "available", out)
    # Free GPIOs 2/3/4/5 are safe on both boards (not reserved board lines).
    out = _run(dev, c, "spi peek", "spi peek 2 3 4 5 0x00", timeout=20)
    c.expect("spi peek reads a byte", "spi.peek:", out)
    out = _run(dev, c, "spi poke", "spi poke 2 3 4 5 0x10 0xAB", timeout=20)
    c.expect("spi poke writes a byte", "spi.poke:", out)
    out = _run(dev, c, "spi loopback", "spi loopback 2 3 4", timeout=20)
    c.expect("spi loopback runs", "spi loopback:", out)
    # The C6 link must still be alive after the SPI bus came up.
    out = _run(dev, c, "ver after spi", "ver", timeout=20)
    c.expect("shell alive after spi init", "P4MiniShell", out)
    _run(dev, c, "spi release", "spi release", timeout=15)

    # -- a couple of visual states --------------------------------------------
    bmp = dev.screenshot(out_dir=OUT_DIR, name="power_audio")
    c.equals("screenshot width", bmp.width, disp_w)
    c.equals("screenshot height", bmp.height, disp_h)
    mean = bmp.region_mean(0, 0, disp_w, disp_h)
    c.check("screen is not uniformly black", sum(mean) > 5, "mean=%r" % (mean,))

    if rgb_ok:
        _run(dev, c, "rgb visual", "rgb 0 0 255", timeout=15)
        _run(dev, c, "rgb off visual", "rgb off", timeout=15)
        _restore_rgb(dev, status)
    bmp2 = dev.screenshot(out_dir=OUT_DIR, name="power_audio_rgb")
    c.equals("second screenshot geometry", (bmp2.width, bmp2.height), (disp_w, disp_h))

    _ = quick
    return c
