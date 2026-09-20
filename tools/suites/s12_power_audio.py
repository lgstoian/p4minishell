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

import os
import re

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
    c.expect("battery detail", "battery.detail:", out)

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
