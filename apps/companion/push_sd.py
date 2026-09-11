#!/usr/bin/env python3
"""push_sd.py - upload the P4 Companion app files to the SD card.

Uses the shell's ACK-paced `receive <path> <size> /crc` binary transfer so
the batch files arrive byte-exact (the device verifies a CRC-32 trailer and
removes the partial file on mismatch).

Usage:
    python push_sd.py [COMx]
"""

import os
import serial
import struct
import sys
import time
import zlib

APP_DIR = os.path.dirname(os.path.abspath(__file__))
FILES = [
    "LIB.BAT",
    "COMPANION.BAT",
    "SYS.BAT",
    "FILES.BAT",
    "NET.BAT",
    "FUN.BAT",
    "SET.BAT",
    "ALIASES.BAT",
    "SVC.BAT",
    "AGENDA.BAT",
    "README.TXT",
    "COMPANION.APPINFO",
]

# push_sd.py writes files to the SD root; the APPINFO metadata belongs in the
# conventional sd:/APPS directory that `launch` reads.
APPINFO_TARGET = "APPS"
CHUNK = 4096
READY = b"=== RX READY ==="
DONE = b"=== RX DONE ==="


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


def push_file(ser, name, data):
    size = len(data)
    ser.reset_input_buffer()
    ser.write(("receive %s %d /crc\n" % (name, size)).encode())
    r = read_until(ser, READY, 10)
    if READY not in r:
        print("  !! no READY for %s (got %r)" % (name, r[-100:]))
        return False
    # READY is printed as "\n=== RX READY ===\n"; consume the trailing newline
    # so it is not mistaken for an ACK.
    read_until(ser, b"\n", 2)

    # ACK-paced: send a chunk, read "RX <cum>\n", then top up from `sent`.
    # `sent` = bytes physically written to the device ring; `cum` = bytes the
    # device ACKed as written to SD. Bytes in the ring beyond `cum` are still
    # buffered by the device, so the next chunk always starts at `sent`.
    sent = 0
    ser.write(data[:CHUNK])
    sent = min(CHUNK, size)
    while True:
        ack = read_until(ser, b"\n", 15)
        if not ack:
            print("  !! no ACK for %s" % name)
            return False
        ack = ack.replace(b"\r", b"")
        if DONE in ack:
            return True
        lines = [l for l in ack.split(b"\n") if l.strip().startswith(b"RX ")]
        if not lines:
            print("  !! bad ACK %r for %s" % (ack[-80:], name))
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
    done = read_until(ser, DONE, 15)
    if DONE not in done:
        print("  !! no DONE for %s (got %r)" % (name, done[-120:]))
        return False
    return True


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM11"
    ser = serial.Serial(port, 115200, timeout=1)
    ser.setDTR(False); ser.setRTS(False)  # open must not reboot the P4
    time.sleep(0.5)
    ser.reset_input_buffer()

    if not wait_shell(ser):
        print("FAIL: shell not responding on %s" % port)
        ser.close()
        return 1

    # The open above usually reboots the board (DTR transition), and boot
    # background work (Wi-Fi init logs) corrupts the ACK-paced protocol if a
    # transfer starts mid-boot. Let the boot quiesce first.
    time.sleep(20.0)
    ser.reset_input_buffer()

    # The APPINFO metadata belongs in the conventional sd:/APPS directory;
    # make sure it exists (ignore "already exists").
    ser.reset_input_buffer()
    ser.write(b"md APPS\n")
    time.sleep(0.5)
    ser.read(ser.in_waiting or 1)

    ok = True
    for name in FILES:
        path = os.path.join(APP_DIR, name)
        target = ("APPS/" + name) if name.endswith(".APPINFO") else name
        with open(path, "rb") as f:
            data = f.read()
        pushed = False
        for attempt in range(3):
            if push_file(ser, target, data):
                print("PUSH ok   %s (%d bytes)" % (target, len(data)))
                pushed = True
                break
            print("  retry %s (%d/3)" % (target, attempt + 1))
            time.sleep(2.0)
        if not pushed:
            print("PUSH FAIL %s" % target)
            ok = False

    ser.close()
    print("PUSH", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
