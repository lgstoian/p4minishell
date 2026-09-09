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

print("Waiting for WiFi boot scan to complete...")
deadline = time.time() + 60
buf = b""
scan_done = False
while time.time() < deadline:
    data = ser.read(4096)
    if data:
        buf += data
        sys.stdout.buffer.write(data)
        sys.stdout.flush()
        # Wait until we see the scan results and then a quiet period
        if b"networks=" in buf and b"[wifi.diag][" in buf:
            # Wait a bit more to ensure scan is fully done
            if not scan_done:
                scan_done = True
                print("\n[SCAN COMPLETE, WAITING 3s]")
                time.sleep(3)
                break
else:
    print("\n[TIMEOUT]")
    ser.close()
    sys.exit(1)

cmd = b"wifi connect 4G-CPE_5542 1234567890\r\n"
print(f"Sending: {cmd.strip().decode()}")
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
