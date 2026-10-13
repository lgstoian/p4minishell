# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""
Two-board chat integration test (sNN_chat.py).

Drives COM3 (host) and COM6 (peer) through the chat verb family:
  COM3: chat host on
  COM6: chat peer <host-ip>
  COM3: chat send "hello"
  COM6: chat poll
  COM6: chat log
  COM3: chat status
  COM3: chat host off
  COM6: chat peer off
"""

from p4test import Device, Checklist, NAME, TAGS


NAME = "chat"
TAGS = ["chat", "integration", "two-board"]


async def run(dev_a: Device, dev_b: Device, ctx) -> Checklist:
    """Run chat integration test between two boards.

    dev_a = host (COM3), dev_b = peer (COM6).
    """
    cl = Checklist()

    # 1. Host starts chat AP
    cl.add("host start", await dev_a.cmd("chat host on"))
    cl.add("host status shows active", await dev_a.cmd("chat status"))

    # 2. Peer connects to host AP
    # Get host IP from status (or use default 192.168.4.1)
    host_ip = "192.168.4.1"  # default AP IP
    cl.add("peer join host", await dev_b.cmd(f"chat peer {host_ip}"))
    cl.add("peer wifi connected", await dev_b.cmd("wifi status"))

    # 3. Host sends message
    cl.add("host send", await dev_a.cmd('chat send "hello peer"'))

    # 4. Peer polls for messages
    cl.add("peer poll receives", await dev_b.cmd("chat poll"))

    # 5. Peer logs
    cl.add("peer log shows", await dev_b.cmd("chat log"))

    # 6. Host status
    cl.add("host status shows peer", await dev_a.cmd("chat status"))

    # 7. Host stops
    cl.add("host stop", await dev_a.cmd("chat host off"))
    cl.add("host status inactive", await dev_a.cmd("chat status"))

    # 8. Peer disconnects
    cl.add("peer disconnect", await dev_b.cmd("chat peer off"))

    return cl