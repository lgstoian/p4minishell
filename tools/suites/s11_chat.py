# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""Chat verb surface (single-board).

The `chat` family talks to a peer over the C6 (TCP), so the full host<->peer
exchange needs two boards and lives in the focused driver
``tools/chat_test.py`` (the suite runner opens one session per board and cannot
pair them). This suite verifies the verb surface, argument validation and the
`onmsg` hook round-trip on one board, without bringing the SoftAP up (an
always-on AP browns out USB-powered boards - bugs.md F35).
"""

from p4test import Checklist


NAME = "chat"
TAGS = ["chat", "core"]


def run(dev, ctx):
    c = Checklist(NAME)

    out = dev.run("chat")
    c.expect("bare chat lists sub-commands", "chat host", out)

    out = dev.run("chat send")
    c.expect("send without text is a usage error", "send <text", out)

    out = dev.run("chat send hello")
    c.expect("send without a peer is refused", "no peer set", out)

    out = dev.run("chat peer")
    c.expect("peer without an IP is a usage error", "host IP required", out)

    out = dev.run("chat onmsg")
    c.expect("onmsg without a line is a usage error", "onmsg <line>", out)

    out = dev.run("chat onmsg echo hi")
    c.expect("onmsg registers the line", "onmsg set to", out)

    out = dev.run("chat onmsg off")
    c.expect("onmsg clears", "onmsg disabled", out)

    out = dev.run("chat status")
    c.check("status responds", len(out.strip()) > 0, "empty output")

    out = dev.run("chat bogus")
    c.expect("unknown sub-command is refused", "unknown sub-command", out)

    return c
