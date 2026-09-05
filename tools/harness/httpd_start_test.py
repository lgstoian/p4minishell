import serial
import time
import sys

port = 'COM11'
baud = 115200

print(f"Opening {port} at {baud} baud...")
ser = serial.Serial(port, baud, timeout=1)
ser.dtr = False
ser.rts = False
ser.reset_input_buffer()

print("Waiting 20 seconds for boot...")
deadline = time.time() + 20
while time.time() < deadline:
    data = ser.read(4096)
    if data:
        sys.stdout.buffer.write(data)
        sys.stdout.flush()

print("\nConnecting WiFi...")
ser.write(b"wifi connect 4G-CPE_5542 1234567890\r\n")
ser.flush()

# Wait for got IP
print("Waiting for IP...")
deadline = time.time() + 45
while time.time() < deadline:
    data = ser.read(4096)
    if data:
        sys.stdout.buffer.write(data)
        sys.stdout.flush()
        if b"got IP" in data:
            print("\n[IP ACQUIRED, WAITING 3s]")
            time.sleep(3)
            break
else:
    print("\n[TIMEOUT waiting for IP]")
    ser.close()
    sys.exit(1)

print("\nStarting httpd...")
ser.write(b"httpd start\r\n")
ser.flush()

print("Reading response for 30 seconds...")
deadline = time.time() + 30
while time.time() < deadline:
    data = ser.read(4096)
    if data:
        sys.stdout.buffer.write(data)
        sys.stdout.flush()

ser.close()
print("\n[TEST COMPLETE]")
