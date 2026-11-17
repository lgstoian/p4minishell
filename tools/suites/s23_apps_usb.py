# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Apps from USB: the same payload runs from a USB stick and its data follows.

v1.4.3 makes every reference app drive-aware: the shell publishes
``APPDRIVE``/``APPROOT``/``APPNAME`` for the running ``.bat`` and the apps use
``%APPDRIVE%`` instead of a hardcoded ``sd:/``. This suite proves the path
end-to-end:

* ``launch /list`` discovers apps under ``usb:/APPS`` (same pattern scan as SD);
* an app installed on USB reports ``APPDRIVE=usb:`` and an absolute
  ``usb:/APPS`` root;
* the same app installed on SD reports ``sd:`` (no cross-drive leakage);
* a real reference app (HYBRID) runs from USB to completion.

The suite needs a mounted USB stick. Without one it fails fast with a clear
message: a silent skip would hide exactly the regression it exists to catch.
"""
from __future__ import annotations

import os

from p4test.asserts import Checklist
from p4test.session import PanicError

NAME = "apps_usb"
TAGS = ["apps", "usb", "slow"]

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
APPS = os.path.join(ROOT, "apps")

# Test fixture (never shipped): reports what the shell published for the run.
PROBE = (
    "@echo off\r\n"
    "echo [M-USBAPP] drive=%APPDRIVE% root=%APPROOT% name=%APPNAME%\r\n"
    "echo [M-USBAPP-DONE]\r\n"
)


def _push_bytes(dev, c, remote, data):
    try:
        dev.push_file(remote, data)
        c.note("pushed %s (%d bytes)" % (remote, len(data)))
        return True
    except PanicError:
        raise
    except Exception as exc:  # noqa: BLE001
        c.check("push %s" % remote, False, str(exc))
        return False


def _push_local(dev, c, local, remote):
    try:
        dev.push_local(local, remote)
        c.note("pushed %s" % remote)
        return True
    except PanicError:
        raise
    except Exception as exc:  # noqa: BLE001
        c.check("push %s" % remote, False, str(exc))
        return False


def _run(dev, c, label, cmd, timeout=60.0):
    try:
        return dev.run(cmd, timeout=timeout)
    except PanicError:
        raise
    except Exception as exc:  # noqa: BLE001
        c.check(label, False, "%r did not return: %s" % (cmd, exc))
        return ""


def _usb_ready(dev, c):
    out = _run(dev, c, "list the USB root", "dir usb:", timeout=25)
    ok = ("error" not in out.lower() and "not mounted" not in out.lower()
          and "no usb" not in out.lower())
    c.check("USB stick mounted (usb: resolves)", ok, out[-300:])
    return ok


def run(dev, ctx):
    c = Checklist(NAME)
    if not _usb_ready(dev, c):
        return c

    _run(dev, c, "usb apps dir", "md usb:/APPS", timeout=20)
    _run(dev, c, "usb web dir", "md usb:/WEB", timeout=20)
    _run(dev, c, "sd apps dir", "md APPS", timeout=20)

    # 1) USB only: the fixture must report the USB root.
    _push_bytes(dev, c, "usb:/APPS/USBROOT.BAT", PROBE.encode())
    out = _run(dev, c, "launch /list sees USB apps", "launch /list", timeout=30)
    c.expect("launch /list lists a USB-installed app", "USBROOT", out)

    out = _run(dev, c, "run USB fixture", "launch USBROOT", timeout=30)
    c.expect("USB app marker", "[M-USBAPP-DONE]", out)
    c.expect("APPDRIVE follows the app's drive (usb:)", "drive=usb:", out)
    c.expect("APPROOT is the app's directory on USB", "root=usb:/APPS", out)
    c.expect("APPNAME is the script stem", "name=USBROOT", out)

    # 2) The same app on SD: `launch` prefers sd:/APPS, so this proves the
    #    variable is recomputed per run instead of leaking the USB value.
    _run(dev, c, "remove the USB fixture", "del /f usb:/APPS/USBROOT.BAT",
         timeout=20)
    _push_bytes(dev, c, "APPS/USBROOT.BAT", PROBE.encode())
    out = _run(dev, c, "run SD fixture", "launch USBROOT", timeout=30)
    c.expect("SD app reports its own drive", "drive=sd:", out)
    c.expect("SD app root", "root=sd:/APPS", out)

    # 3) A real reference app from USB.
    for name, sub in (("APPLIB.BAT", ""), ("HYBRID.BAT", "hybrid"),
                      ("HYBRID.APPINFO", "hybrid")):
        local = os.path.join(APPS, sub, name) if sub else os.path.join(APPS, name)
        remote = ("usb:/APPS/" if name.endswith(".APPINFO") else "usb:/") + name
        _push_local(dev, c, local, remote)

    out = _run(dev, c, "run HYBRID from USB", "launch HYBRID", timeout=90)
    c.expect("reference app starts from USB", "[M-HYBRID]", out)
    c.expect("reference app completes from USB", "[M-HYBRID-DONE]", out)

    _run(dev, c, "cleanup sd fixture", "del /f APPS/USBROOT.BAT", timeout=20)
    return c
