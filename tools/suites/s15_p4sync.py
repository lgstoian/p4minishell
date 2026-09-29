# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""P4Sync conformance suite: USB desktop-sync handshake and full-image paths.

Proves, through the existing transports only, everything the future P4Sync
desktop app needs: the read-only `sync status` handshake (`sync.*` lines),
a small file push/pull round-trip over `receive`/`send` (SDFX), and a
screenshot whose geometry matches the live display (BMPX). PIM merge,
package install, and asset checks stay with their owning suites (`s07_data`,
`s14_apps`); this suite only asserts the P4Sync handshake advertises the
magics and limits those paths depend on.

Scratch files live under `sd:/P4SYNC` and are removed afterwards.
"""
import time

from p4test.asserts import Checklist

NAME = "p4sync"
TAGS = ["core", "sd"]

SCRATCH_DIR = "sd:/P4SYNC"


def run(dev, ctx):
    c = Checklist(NAME)
    stamp = "%05d" % (int(time.time()) % 100000)
    remote = "%s/T%s.TXT" % (SCRATCH_DIR, stamp)
    payload = ("p4sync-%s\n" % stamp).encode() * 64

    # --- handshake ----------------------------------------------------
    out = dev.run("sync status")
    c.expect("sync proto", "sync.proto:", out)
    c.expect("sync proto tag", "p4sync-1", out)
    c.expect("sync board", "sync.board:", out)
    c.expect("sync sd", "sync.sd:", out)
    c.expect("sync lock", "sync.lock:", out)
    c.expect("sync send magic", "SDFX", out)
    c.expect("sync pim magic", "PIMX", out)
    c.expect("sync shot magic", "BMPX", out)
    c.expect("sync send max", "sync.send_max:", out)
    c.expect("sync rx max", "sync.rx_max:", out)
    c.expect("sync pim rx max", "sync.pim_rx_max:", out)
    c.expect("sync inventory max", "sync.inventory_max:", out)
    c.expect("sync ready", "sync: ready", out)
    c.equals("sync board matches sysinfo", dev.board_id() in out, True)

    out = dev.run("sync")
    c.expect("sync bare is status", "sync.proto:", out)

    out = dev.run("sync bogus")
    c.expect("sync usage", "Usage: sync", out)

    # --- file push/pull over the shared serial engine ------------------
    dev.run("md %s" % SCRATCH_DIR)
    dev.push_file(remote, payload)
    back = dev.pull_file(remote)
    c.equals("file round-trip", back, payload)

    # --- screenshot geometry matches the live display -------------------
    w, h = dev.display_size()
    shot = dev.screenshot()
    c.equals("shot width", shot.width, w)
    c.equals("shot height", shot.height, h)

    # --- cleanup ---------------------------------------------------------
    dev.run("del /f %s" % remote)
    return c
