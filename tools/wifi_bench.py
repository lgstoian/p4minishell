#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""wifi_bench.py - Wi-Fi throughput bench (host endpoint for `wifi throughput`).

Drives the firmware's `wifi throughput` command and runs the matching host
endpoint, so one invocation measures the ESP-Hosted SDIO transport in either
direction and prints both the host- and device-side numbers.

Modes are named from the host's point of view:
  rx  host RECEIVES <- device sends   (runs `wifi throughput tx <host-ip>`)
  tx  host SENDS    -> device receives (runs `wifi throughput rx`)

Usage:
  python tools/wifi_bench.py [COMx] rx|tx [mb] [--udp] [--port N]
"""

import os
import re
import socket
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from shell_session import open_port, default_port  # noqa: E402

DEFAULT_MB = 16
DEFAULT_PORT = 5201
CHUNK = 65536
ANSI = re.compile(r"\x1b\[[0-9;]*m")


def device_ip(ser):
    ser.reset_input_buffer()
    ser.write(b"wifi status\n")
    buf = b""
    end = time.time() + 8
    while time.time() < end:
        d = ser.read(65536)
        if d:
            buf += d
            if b"IPv4:" in buf:
                break
        else:
            time.sleep(0.05)
    text = ANSI.sub("", buf.decode(errors="replace"))
    m = re.search(r"IPv4:\s*(\d+\.\d+\.\d+\.\d+)", text)
    return m.group(1) if m else None


def local_ip_for(remote_ip, port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((remote_ip, port))
        return s.getsockname()[0]
    finally:
        s.close()


def run_device_command(ser, command, results, timeout):
    ser.reset_input_buffer()
    ser.write((command + "\n").encode())
    buf = b""
    end = time.time() + timeout
    while time.time() < end:
        d = ser.read(65536)
        if not d:
            time.sleep(0.05)
            continue
        buf += d
        if b"Mbit/s" in buf or b"bench failed" in buf:
            break
    results["device"] = ANSI.sub("", buf.decode(errors="replace"))


def host_receive(port, mb, udp, results, timeout):
    """Host listens; device sends. Timer starts at first byte."""
    target = mb * 1024 * 1024
    start = None
    got = 0
    if udp:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("0.0.0.0", port))
        s.settimeout(timeout)
        try:
            while got < target:
                d, _ = s.recvfrom(CHUNK)
                if start is None:
                    start = time.time()
                got += len(d)
        except socket.timeout:
            pass
        finally:
            s.close()
    else:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("0.0.0.0", port))
        s.listen(1)
        s.settimeout(timeout)
        try:
            c, _ = s.accept()
            c.settimeout(timeout)
            while got < target:
                d = c.recv(CHUNK)
                if not d:
                    break
                if start is None:
                    start = time.time()
                got += len(d)
            c.close()
        except socket.timeout:
            pass
        finally:
            s.close()
    elapsed = (time.time() - start) if start else 0.0
    results["host"] = (got, elapsed)


def host_send(host, port, mb, udp, results, timeout):
    """Host sends to the (already listening) device."""
    target = mb * 1024 * 1024
    payload = b"x" * 1460
    sent = 0
    start = None
    if udp:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        deadline = time.time() + timeout
        while sent < target and time.time() < deadline:
            try:
                s.sendto(payload, (host, port))
            except OSError:
                break
            if start is None:
                start = time.time()
            sent += len(payload)
        s.close()
    else:
        # The device may still be binding; retry the connect briefly.
        deadline = time.time() + timeout
        c = None
        while time.time() < deadline:
            try:
                c = socket.create_connection((host, port), timeout=2)
                break
            except OSError:
                time.sleep(0.2)
        if c is not None:
            c.settimeout(timeout)
            while sent < target:
                try:
                    n = c.send(payload)
                except OSError:
                    break
                if start is None:
                    start = time.time()
                sent += n
            c.close()
    elapsed = (time.time() - start) if start else 0.0
    results["host"] = (sent, elapsed)


def main():
    args = sys.argv[1:]
    port_name = default_port()
    positional = []
    udp = False
    tcp_port = DEFAULT_PORT
    mb = None
    i = 0
    while i < len(args):
        a = args[i]
        if a.upper().startswith("COM"):
            port_name = a.upper()
        elif a == "--udp":
            udp = True
        elif a == "--port":
            i += 1
            tcp_port = int(args[i])
        elif a.lower() in ("rx", "tx"):
            positional.append(a.lower())
        else:
            mb = int(a)
        i += 1

    if len(positional) != 1:
        print(__doc__)
        return 2
    direction = positional[0]
    mb = mb or DEFAULT_MB

    results = {}
    ser = open_port(port_name, 115200, 1)
    time.sleep(12.0)  # boot quiesce / let Wi-Fi associate
    ip = device_ip(ser)
    if ip is None:
        print("FAIL: could not read the device IPv4 from `wifi status`")
        ser.close()
        return 1
    host_ip = local_ip_for(ip, tcp_port)
    print("device %s, host %s, port %d, %d MiB, %s" %
          (ip, host_ip, tcp_port, mb, "udp" if udp else "tcp"))

    timeout = max(30, mb)  # generous per-MiB budget

    if direction == "rx":
        # Host receives; device transmits.
        cmd = "wifi throughput tx %s port=%d mb=%d%s" % (
            host_ip, tcp_port, mb, " udp" if udp else "")
        t = threading.Thread(target=host_receive,
                             args=(tcp_port, mb, udp, results, timeout))
        t.start()
        time.sleep(0.5)
        run_device_command(ser, cmd, results, timeout + 10)
        t.join()
    else:
        # Host sends; device receives. Start the device side first so it binds.
        cmd = "wifi throughput rx port=%d mb=%d%s" % (
            tcp_port, mb, " udp" if udp else "")
        dt = threading.Thread(target=run_device_command,
                              args=(ser, cmd, results, timeout + 10))
        dt.start()
        time.sleep(1.0)
        host_send(ip, tcp_port, mb, udp, results, timeout)
        dt.join()

    ser.close()

    got, elapsed = results.get("host", (0, 0.0))
    host_mbps = (got * 8 / elapsed / 1e6) if elapsed > 0 else 0.0
    print("host: %s %.2f Mbit/s (%d bytes in %.2fs)" %
          (direction, host_mbps, got, elapsed))
    print("--- device ---")
    print(results.get("device", "(no device output)"))
    return 0


sys.exit(main())
