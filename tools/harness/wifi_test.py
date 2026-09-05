import serial
import time
import sys

port = 'COM11'
baud = 115200

print(f"Opening {port} at {baud} baud...")
with serial.Serial(port, baud, timeout=1) as ser:
    ser.reset_input_buffer()
    ser.reset_output_buffer()
    
    print("Waiting for shell prompt...")
    deadline = time.time() + 30
    buf = b""
    while time.time() < deadline:
        data = ser.read(1024)
        if data:
            buf += data
            sys.stdout.buffer.write(data)
            sys.stdout.flush()
            if b"> " in buf:
                print("\n[PROMPT FOUND]")
                break
    else:
        print("\n[TIMEOUT waiting for prompt]")
        sys.exit(1)
    
    cmd = b"wifi connect 4G-CPE_5542 1234567890\r\n"
    print(f"Sending: {cmd.strip().decode()}")
    ser.write(cmd)
    ser.flush()
    
    print("Reading response for 45 seconds...")
    deadline = time.time() + 45
    while time.time() < deadline:
        data = ser.read(1024)
        if data:
            sys.stdout.buffer.write(data)
            sys.stdout.flush()
    
    print("\n[TEST COMPLETE]")
