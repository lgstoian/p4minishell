import serial
import time
import os

port = "COM11"
OUTPUT_DIR = r"D:\p4minishell\screenshots"
os.makedirs(OUTPUT_DIR, exist_ok=True)

try:
    ser = serial.Serial(port, 115200, timeout=2)
    time.sleep(1)
    
    if ser.in_waiting:
        ser.read(ser.in_waiting)
    
    # Save screenshot to SD card
    print("Saving screenshot to SD card...")
    ser.write(b"screenshot screen.bmp\r\n")
    
    # Wait for completion
    data = b''
    start = time.time()
    while time.time() - start < 20:
        if ser.in_waiting:
            chunk = ser.read(ser.in_waiting)
            data += chunk
            text = chunk.decode('utf-8', errors='replace')
            if "saved" in text.lower() or "error" in text.lower():
                print(text.strip())
                break
        time.sleep(0.1)
    
    ser.close()
    print("Done - check SD card for screen.bmp")
    
except Exception as e:
    print(f"Error: {e}")
