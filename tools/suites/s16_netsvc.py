# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Event-service suite: the `net` verbs without needing a live broker.

Covers the offline-safe surface (status/subs/msg/outbox introspection, the
RAM-only subscription table and `/onmsg` hook, usage errors) on every run.
Live broker checks (connect, publish flush, inbound loopback) run only when
a broker answers; otherwise they are recorded as notes, never failures —
like s11's disconnected-Wi-Fi notes. Fixture: a LAN Mosquitto on 1883 with
anonymous access, pointed at via `net broker` before the run.
"""
import time

from p4test.asserts import Checklist

NAME = "netsvc"
TAGS = ["network", "slow"]

FILTER = "p4test/netsvc/%05d" % (int(time.time()) % 100000)


def run(dev, ctx):
    c = Checklist(NAME)

    # --- offline-safe introspection (no broker, no Wi-Fi needed) ---------
    out = dev.run("net status")
    c.expect("net state line", "net.state:", out)
    c.expect("net subs count", "net.subs:", out)
    c.expect("net outbox count", "net.outbox:", out)

    out = dev.run("net status /b")
    lines = [l for l in out.splitlines() if l.strip() != ""]
    c.check("status /b has three lines", len(lines) >= 3, out[-200:])

    out = dev.run("net subs")
    c.expect("subs empty note", "no subscriptions", out)

    out = dev.run("net msg")
    c.expect("msg empty note", "no message yet", out)

    out = dev.run("net bogus")
    c.expect("usage on unknown verb", "Usage: net", out)

    out = dev.run("net pub")
    c.expect("usage on bare pub", "Usage: net", out)

    out = dev.run("net sub")
    c.expect("usage on bare sub", "Usage: net", out)

    # --- RAM-only table + hook round-trip (no broker needed) -------------
    out = dev.run("net sub %s" % FILTER)
    c.expect("sub stored", FILTER, out)
    out = dev.run("net subs /b")
    c.expect("subs lists filter", FILTER, out)
    out = dev.run("net unsub %s" % FILTER)
    c.expect("unsub stored", FILTER, out)
    out = dev.run("net subs /b")
    c.expect("subs removed", FILTER, out, want=False)

    out = dev.run("net onmsg echo arrived $NET_TOPIC")
    c.expect("hook stored", "hook set", out)
    out = dev.run("net onmsg off")
    c.expect("hook cleared", "hook cleared", out)

    # --- live broker checks (notes when unreachable) ---------------------
    dev.run("net disconnect")
    out = dev.run("net connect", timeout=30)
    live = False
    for _ in range(12):
        time.sleep(5)
        st = dev.run("net status /b")
        if "connected" in st.splitlines()[0]:
            live = True
            break
    if not live:
        c.note("no broker reachable: live publish/loopback skipped")
        dev.run("net disconnect")
        return c

    c.expect("connected state", "connected", dev.run("net status"))
    dev.run("net sub %s" % FILTER)
    out = dev.run("net pub %s P4NETLOOP" % FILTER, timeout=60)
    c.expect("publish live", "live", out)
    got = ""
    for _ in range(12):
        time.sleep(5)
        got = dev.run("net msg /b", timeout=30)
        if "P4NETLOOP" in got:
            break
    c.expect("inbound loopback", "P4NETLOOP", got)

    # --- cleanup ----------------------------------------------------------
    dev.run("net unsub %s" % FILTER)
    dev.run("net onmsg off")
    dev.run("net disconnect")
    return c
