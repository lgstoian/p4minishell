# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
import time
import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from shell_session import open_port, default_port

port = default_port()
baud = 115200

print(f"Opening {port} at {baud} baud...")
ser = open_port(port, baud, timeout=1)
ser.reset_input_buffer()

print("Waiting 20 seconds for boot to complete...")
deadline = time.time() + 20
while time.time() < deadline:
    data = ser.read(4096)
    if data:
        sys.stdout.buffer.write(data)
        sys.stdout.flush()

cmd = b"wifi connect <P4_WIFI_SSID> <P4_WIFI_PASSWORD>\r\n"
print(f"\nSending: {cmd.strip().decode()}")
ser.write(cmd)
ser.flush()

print("Reading response for 60 seconds...")
deadline = time.time() + 60
while time.time() < deadline:
    data = ser.read(4096)
    if data:
        sys.stdout.buffer.write(data)
        sys.stdout.flush()

ser.close()
print("\n[TEST COMPLETE]")
