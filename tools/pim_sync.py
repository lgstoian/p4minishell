#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""pim_sync.py - serial CardDAV-lite sync for contacts (vCard) and alarms (iCal).

Talks to the device `pim` command over USB-Serial/JTAG:
  pull  - `pim get db <name> | pim get alarms` -> PIMX frame -> mirror dir
  push  - mirror file(s) -> `pim put ... <size> /crc` (ACK-paced, CRC trailer)
  sync  - pull, merge by UID with newer REV/DTSTAMP winning, push back if
          the device side changed, and refresh the mirror dir.

The mirror dir holds one card per file (`<uid>.vcf` / `<uid>.ics`) so a
plain folder *is* the host-side store; no database needed.

Framing mirrors tools/pull.py (PIMX magic + LE32 size + payload + LE32 CRC,
verified here with zlib.crc32) and apps/companion/push_sd.py (READY marker,
4 KB ACK-paced chunks, CRC trailer, DONE marker, 3 retries).

Sync assumes the host and the device share wall-clock zone: the device
renders floating local times and this tool reads/writes them as host-local.

Usage:
    python tools/pim_sync.py [--port COMx] pull db NOTES ./mirror-notes
    python tools/pim_sync.py [--port COMx] pull alarms ./mirror-alarms
    python tools/pim_sync.py [--port COMx] push db NOTES ./mirror-notes
    python tools/pim_sync.py [--port COMx] sync alarms ./mirror-alarms
"""

import argparse
import calendar
import os
import re
import struct
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from shell_session import open_port  # noqa: E402

MAGIC = b"PIMX"
READY = b"=== RX READY ==="
DONE = b"=== RX DONE ==="
TX_DONE = b"=== TX DONE ==="
CHUNK = 4096


def read_until(ser, marker, timeout=15.0):
    buf = b""
    end = time.time() + timeout
    while time.time() < end:
        data = ser.read(ser.in_waiting or 1)
        if data:
            buf += data
            if marker in buf:
                return buf
    return buf


def wait_shell(ser):
    end = time.time() + 10
    while time.time() < end:
        ser.write(b"echo ready\n")
        time.sleep(0.3)
        if b"ready" in ser.read(ser.in_waiting or 1):
            return True
    return False


def connect(port):
    ser = open_port(port, 115200, 1)
    ser.reset_input_buffer()
    if not wait_shell(ser):
        print("FAIL: shell not responding on %s" % port)
        return None
    # A fresh open usually reboots the board; let boot logs quiesce so they
    # cannot corrupt the ACK-paced protocol (same caution as push_sd.py).
    time.sleep(20.0)
    ser.reset_input_buffer()
    return ser


def pull_frame(ser, cmd):
    """Run a `pim get ...` command and return the verified PIMX payload."""
    ser.reset_input_buffer()
    ser.write((cmd + "\n").encode())
    blob = read_until(ser, MAGIC, 30)
    at = blob.find(MAGIC)
    if at < 0:
        print("  !! no PIMX magic for %r (got %r)" % (cmd, blob[-120:]))
        return None
    rest = blob[at + 4:]
    end = time.time() + 15.0
    while len(rest) < 4 and time.time() < end:
        more = ser.read(4 - len(rest))
        if more:
            rest += more
        else:
            time.sleep(0.05)
    if len(rest) < 4:
        print("  !! no PIMX size for %r" % cmd)
        return None
    (size,) = struct.unpack("<I", rest[:4])
    if size == 0 or size > 16 * 1024 * 1024:
        print("  !! implausible PIMX size %d" % size)
        return None
    data = rest[4:]
    end = time.time() + 120.0
    while len(data) < size + 4 and time.time() < end:
        chunk = ser.read(min(65536, size + 4 - len(data)))
        if chunk:
            data += chunk
        else:
            time.sleep(0.05)
    if len(data) < size + 4:
        print("  !! short PIMX payload (%d/%d)" % (len(data), size + 4))
        return None
    payload, (crc,) = data[:size], struct.unpack("<I", data[size:size + 4])
    if (zlib.crc32(payload) & 0xFFFFFFFF) != crc:
        print("  !! PIMX CRC mismatch")
        return None
    return payload


def push_stream(ser, cmd, data, label):
    """Run a `pim put ... <size> /crc` command with ACK-paced streaming."""
    size = len(data)
    ser.reset_input_buffer()
    ser.write(("%s %d /crc\n" % (cmd, size)).encode())
    r = read_until(ser, READY, 15)
    if READY not in r:
        print("  !! no READY for %s (got %r)" % (label, r[-100:]))
        return False
    # READY is printed as "\n=== RX READY ===\n"; consume the trailing
    # newline so it is not mistaken for an ACK.
    read_until(ser, b"\n", 2)

    sent = 0
    ser.write(data[:CHUNK])
    sent = min(CHUNK, size)
    while True:
        ack = read_until(ser, b"\n", 15)
        if not ack:
            print("  !! no ACK for %s" % label)
            return False
        ack = ack.replace(b"\r", b"")
        if DONE in ack:
            return True
        lines = [l for l in ack.split(b"\n") if l.strip().startswith(b"RX ")]
        if not lines:
            print("  !! bad ACK %r for %s" % (ack[-80:], label))
            return False
        cum = int(lines[-1].strip()[3:])
        if cum >= size:
            break
        if sent < size:
            nxt = min(sent + CHUNK, size)
            ser.write(data[sent:nxt])
            sent = nxt

    crc = zlib.crc32(data) & 0xFFFFFFFF
    ser.write(struct.pack("<I", crc))
    # Merging (db writes) happens before the device prints DONE; be patient.
    done = read_until(ser, DONE, 120)
    if DONE not in done:
        print("  !! no DONE for %s (got %r)" % (label, done[-120:]))
        return False
    return True


# ---------------------------------------------------------------------------
# Card splitting / identity parsing (shared by vcf and ics)
# ---------------------------------------------------------------------------

def unfold(text):
    """Unfold continuation lines (leading SP/HTAB appends to previous)."""
    out = []
    for line in text.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        if line[:1] in (" ", "\t") and out:
            out[-1] += line[1:]
        else:
            out.append(line)
    return out


def split_blocks(payload, begin, end):
    """Split a collection into (start, end) line ranges of BEGIN..END."""
    lines = unfold(payload.decode("utf-8", "replace"))
    blocks, cur, start = [], None, 0
    for i, line in enumerate(lines):
        if line.strip().upper() == begin:
            cur, start = i, i
        elif line.strip().upper() == end and cur is not None:
            blocks.append((start, i))
            cur = None
    return lines, blocks


def block_uid(lines, start, end):
    for i in range(start, end + 1):
        m = re.match(r"(?i)^(?:[A-Za-z0-9-]+\.)?UID:(.*)$", lines[i].strip())
        if m:
            return m.group(1).strip()
    return None


def block_rev(lines, start, end):
    """Newest-wins stamp: REV for vCard, DTSTAMP for VEVENT (epoch, else 0)."""
    for i in range(start, end + 1):
        m = re.match(r"(?i)^(?:[A-Za-z0-9-]+\.)?(?:REV|DTSTAMP):(.*)$",
                     lines[i].strip())
        if m:
            return parse_stamp(m.group(1).strip())
    return 0


def parse_stamp(text):
    """Parse YYYYMMDDTHHMMSS[Z] (or date-only) into an epoch (0 on failure)."""
    m = re.match(r"^(\d{4})(\d{2})(\d{2})(?:T(\d{2})(\d{2})(\d{2}))?([Zz])?$",
                 text.strip())
    if not m:
        return 0
    y, mo, d = int(m.group(1)), int(m.group(2)), int(m.group(3))
    h, mi, s = (int(m.group(4) or 0), int(m.group(5) or 0),
                int(m.group(6) or 0))
    try:
        if m.group(7):
            return calendar.timegm((y, mo, d, h, mi, s, 0, 0, 0))
        return int(time.mktime((y, mo, d, h, mi, s, 0, 0, -1)))
    except (OverflowError, ValueError):
        return 0


def format_stamp(epoch):
    return time.strftime("%Y%m%dT%H%M%S", time.localtime(epoch))


def safe_name(uid):
    return re.sub(r"[^A-Za-z0-9_.@-]+", "_", uid)[:80] or "noid"


def load_mirror(mirror, ext):
    """Mirror dir -> {uid: (rev, card_text)} from per-uid files."""
    cards = {}
    if not os.path.isdir(mirror):
        return cards
    for fname in sorted(os.listdir(mirror)):
        if not fname.endswith(ext):
            continue
        with open(os.path.join(mirror, fname), "rb") as f:
            text = f.read().decode("utf-8", "replace")
        lines = unfold(text)
        uid = block_uid(lines, 0, len(lines) - 1) or fname[:-len(ext)]
        cards[uid] = (block_rev(lines, 0, len(lines) - 1), text)
    return cards


def collection_cards(payload, begin, end):
    """PIMX payload -> {uid: (rev, card_text)} (uid-less cards get a key)."""
    lines, blocks = split_blocks(payload, begin, end)
    cards = {}
    for n, (s, e) in enumerate(blocks):
        uid = block_uid(lines, s, e) or "noid-%d" % n
        cards[uid] = (block_rev(lines, s, e),
                      "\r\n".join(lines[s:e + 1]) + "\r\n")
    return cards


def stamp_card(text, uid, rev, kind):
    """Ensure a card carries UID + REV/DTSTAMP (host-minted when missing)."""
    lines = unfold(text)
    has_uid = any(re.match(r"(?i)^(?:[A-Za-z0-9-]+\.)?UID:", l.strip())
                  for l in lines)
    has_rev = any(re.match(r"(?i)^(?:[A-Za-z0-9-]+\.)?(?:REV|DTSTAMP):",
                           l.strip()) for l in lines)
    prop = "DTSTAMP" if kind == "ics" else "REV"
    out = []
    for i, l in enumerate(lines):
        out.append(l)
        if i == 1 and not has_uid:
            out.append("UID:" + uid)
            if not has_rev:
                out.append("%s:%s" % (prop, format_stamp(rev)))
        elif i == 1 and not has_rev:
            out.append("%s:%s" % (prop, format_stamp(rev)))
    if len(lines) < 2:
        # Degenerate single-line card: append identity at the end.
        if not has_uid:
            out.append("UID:" + uid)
        if not has_rev:
            out.append("%s:%s" % (prop, format_stamp(rev)))
    return "\r\n".join(out) + "\r\n"


def write_mirror(mirror, ext, cards, kind):
    os.makedirs(mirror, exist_ok=True)
    for uid, (rev, text) in sorted(cards.items()):
        if rev <= 0:
            rev = int(time.time())
            text = stamp_card(text, uid, rev, kind)
            cards[uid] = (rev, text)
        with open(os.path.join(mirror, safe_name(uid) + ext), "wb") as f:
            f.write(text.encode("utf-8"))


def join_collection(cards, kind):
    if kind == "vcf":
        return "".join(t for _, t in sorted(cards.values())).encode("utf-8")
    body = "".join(t for _, t in sorted(cards.values()))
    return ("BEGIN:VCALENDAR\r\nVERSION:2.0\r\n" + body +
            "END:VCALENDAR\r\n").encode("utf-8")


# ---------------------------------------------------------------------------
# Verbs
# ---------------------------------------------------------------------------

def cmd_pull(ser, kind, name, mirror):
    ext = ".ics" if kind == "ics" else ".vcf"
    cmd = "pim get alarms" if kind == "ics" else "pim get db %s" % name
    payload = None
    for attempt in range(3):
        payload = pull_frame(ser, cmd)
        if payload is not None:
            break
        print("  retry pull (%d/3)" % (attempt + 1))
        time.sleep(2.0)
    if payload is None:
        print("PULL FAIL")
        return 1
    begin, end = ("BEGIN:VEVENT", "END:VEVENT") if kind == "ics" else \
        ("BEGIN:VCARD", "END:VCARD")
    cards = collection_cards(payload, begin, end)
    write_mirror(mirror, ext, cards, kind)
    print("PULL ok %d card(s) -> %s" % (len(cards), mirror))
    return 0


def read_push_bytes(kind, src):
    if os.path.isdir(src):
        cards, ext = {}, ".ics" if kind == "ics" else ".vcf"
        for fname in sorted(os.listdir(src)):
            if fname.endswith(ext):
                with open(os.path.join(src, fname), "rb") as f:
                    cards[fname] = (0, f.read().decode("utf-8", "replace"))
        return join_collection(cards, kind)
    with open(src, "rb") as f:
        return f.read()


def cmd_push(ser, kind, name, src):
    cmd = "pim put alarms" if kind == "ics" else "pim put db %s" % name
    try:
        data = read_push_bytes(kind, src)
    except OSError as e:
        print("PUSH FAIL: cannot read %s (%s)" % (src, e))
        return 1
    if not data:
        print("PUSH FAIL: nothing to push")
        return 1
    label = "%s (%d bytes)" % (src, len(data))
    for attempt in range(3):
        if push_stream(ser, cmd, data, label):
            print("PUSH ok   %s" % label)
            return 0
        print("  retry %s (%d/3)" % (label, attempt + 1))
        time.sleep(2.0)
    print("PUSH FAIL %s" % label)
    return 1


def cmd_sync(ser, kind, name, mirror):
    ext = ".ics" if kind == "ics" else ".vcf"
    cmd = "pim get alarms" if kind == "ics" else "pim get db %s" % name
    payload = None
    for attempt in range(3):
        payload = pull_frame(ser, cmd)
        if payload is not None:
            break
        print("  retry pull (%d/3)" % (attempt + 1))
        time.sleep(2.0)
    if payload is None:
        print("SYNC FAIL: pull failed")
        return 1
    begin, end = ("BEGIN:VEVENT", "END:VEVENT") if kind == "ics" else \
        ("BEGIN:VCARD", "END:VCARD")
    dev = collection_cards(payload, begin, end)
    local = load_mirror(mirror, ext)
    merged = dict(local)
    for uid, (rev, text) in dev.items():
        if uid not in merged or rev >= merged[uid][0]:
            merged[uid] = (rev, text)
    # Host-created cards (no device copy) are stamped so the device keeps
    # them on the push-back; stamp here, not on every mirror write.
    now = int(time.time())
    for uid in list(merged):
        rev, text = merged[uid]
        if uid not in dev and rev <= 0:
            merged[uid] = (now, stamp_card(text, uid, now, kind))
    write_mirror(mirror, ext, merged, kind)
    # Push back only what the device does not already have at this rev.
    push = {u: v for u, v in merged.items()
            if u not in dev or v[0] > dev[u][0]}
    if not push:
        print("SYNC ok, device already current (%d card(s))" % len(merged))
        return 0
    data = join_collection(push, kind)
    put = "pim put alarms" if kind == "ics" else "pim put db %s" % name
    for attempt in range(3):
        if push_stream(ser, put, data, "%d changed card(s)" % len(push)):
            print("SYNC ok, pushed %d changed card(s) (%d total)" %
                  (len(push), len(merged)))
            return 0
        print("  retry push (%d/3)" % (attempt + 1))
        time.sleep(2.0)
    print("SYNC FAIL: push failed")
    return 1


def main(argv=None):
    ap = argparse.ArgumentParser(description="Serial PIM sync (PIMX).")
    ap.add_argument("--port", default="COM11")
    ap.add_argument("verb", choices=["pull", "push", "sync"])
    ap.add_argument("kind", choices=["db", "alarms"])
    ap.add_argument("name", nargs="?",
                    help="db name (required for kind=db)")
    ap.add_argument("path", help="mirror dir (pull/sync) or file-or-dir (push)")
    args = ap.parse_args(argv)
    if args.kind == "db" and not args.name:
        ap.error("db name required for kind=db")

    ser = connect(args.port)
    if ser is None:
        return 1
    try:
        if args.verb == "pull":
            return cmd_pull(ser, "ics" if args.kind == "alarms" else "vcf",
                            args.name, args.path)
        if args.verb == "push":
            return cmd_push(ser, "ics" if args.kind == "alarms" else "vcf",
                            args.name, args.path)
        return cmd_sync(ser, "ics" if args.kind == "alarms" else "vcf",
                        args.name, args.path)
    finally:
        ser.close()


if __name__ == "__main__":
    sys.exit(main())
