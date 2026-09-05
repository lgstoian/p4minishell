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

print("Waiting 20 seconds for boot to complete...")
deadline = time.time() + 20
while time.time() < deadline:
    data = ser.read(4096)
    if data:
        sys.stdout.buffer.write(data)
        sys.stdout.flush()

cmd = b"wifi connect 4G-CPE_5542 1234567890\r\n"
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
