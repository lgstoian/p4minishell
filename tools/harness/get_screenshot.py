"""
Get screenshot from P4MiniShell and save to project folder.
Reads the BMP file from SD card via serial console.
"""
import time
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from shell_session import open_port, default_port

port = default_port()
OUTPUT_DIR = os.path.join(os.getcwd(), "screenshots")
os.makedirs(OUTPUT_DIR, exist_ok=True)

def read_bmp_from_sd():
    """Read the BMP file from SD card using serial console."""
    ser = open_port(port, 115200, timeout=2)
    time.sleep(2)
    
    # Clear any pending data
    ser.read(ser.in_waiting if ser.in_waiting else 0)
    
    # First, check the file size
    print("Checking file on SD card...")
    ser.write(b"dir screen.bmp\r\n")
    time.sleep(2)
    
    dir_data = b''
    while ser.in_waiting:
        dir_data += ser.read(ser.in_waiting)
    
    print("Directory listing:")
    print(dir_data.decode('utf-8', errors='replace'))
    
    # Now read the file using type command
    # The type command outputs raw bytes
    print("\nReading file from SD card...")
    ser.write(b"type screen.bmp\r\n")
    
    # Collect all data
    all_data = b''
    start_time = time.time()
    
    # Wait for data and collect it
    while time.time() - start_time < 10:  # 10 second timeout
        chunk = ser.read(4096 if ser.in_waiting else 1)
        if chunk:
            all_data += chunk
    
    ser.close()
    
    return all_data

def extract_bmp_from_serial(raw_data):
    """Extract BMP data from serial output, filtering ANSI codes."""
    bmp_data = b''
    i = 0
    
    while i < len(raw_data):
        byte = raw_data[i]
        
        # Skip ANSI escape sequences
        if byte == 0x1b and i + 1 < len(raw_data):
            # Check if it's an SGR sequence (ESC[...m)
            if raw_data[i + 1] == ord('['):
                # Find the 'm' terminator
                j = i + 2
                while j < len(raw_data) and raw_data[j] != ord('m'):
                    j += 1
                if j < len(raw_data):
                    i = j + 1  # Skip past the 'm'
                    continue
        
        # Include printable characters and newlines
        if byte >= 0x20 or byte in (0x0a, 0x0d):
            bmp_data += bytes([byte])
        
        i += 1
    
    return bmp_data

def main():
    print("=== P4MiniShell Screenshot Grabber ===\n")
    
    raw_data = read_bmp_from_sd()
    print(f"\nRaw serial data received: {len(raw_data)} bytes")
    
    # Extract BMP data (filter out ANSI codes)
    bmp_data = extract_bmp_from_serial(raw_data)
    print(f"Extracted BMP data: {len(bmp_data)} bytes")
    
    # Verify BMP header
    if len(bmp_data) < 54:
        print("ERROR: Data too short for BMP header")
        return
    
    # BMP signature check
    if bmp_data[0:2] != b'BM':
        print(f"ERROR: Invalid BMP signature: {bmp_data[0:2]}")
        # Try to find the start of actual data
        idx = bmp_data.find(b'BM')
        if idx >= 0:
            print(f"Found BMP signature at offset {idx}")
            bmp_data = bmp_data[idx:]
        else:
            print("No BMP signature found")
            return
    
    # Parse BMP header
    width = int.from_bytes(bmp_data[18:22], 'little')
    height = int.from_bytes(bmp_data[22:26], 'little')
    bpp = int.from_bytes(bmp_data[28:30], 'little')
    
    print(f"\nBMP Properties:")
    print(f"  Width: {width} pixels")
    print(f"  Height: {height} pixels")
    print(f"  Bits per pixel: {bpp}")
    print(f"  File size: {len(bmp_data)} bytes")
    
    # Expected size: 54 bytes header + width * height * (bpp/8)
    expected_size = 54 + width * height * (bpp // 8)
    print(f"  Expected size: {expected_size} bytes")
    
    if len(bmp_data) != expected_size:
        print(f"  WARNING: Size mismatch (got {len(bmp_data)}, expected {expected_size})")
    
    # Save the file
    timestamp = time.strftime("%Y%m%d_%H%M%S")
    filename = os.path.join(OUTPUT_DIR, f"screenshot_{timestamp}.bmp")
    
    with open(filename, 'wb') as f:
        f.write(bmp_data)
    
    print(f"\nSUCCESS: Saved to {filename}")
    print(f"File size: {len(bmp_data)} bytes")
    
    # Also save a copy named screen.bmp for easy access
    copy_name = os.path.join(OUTPUT_DIR, "screen.bmp")
    with open(copy_name, 'wb') as f:
        f.write(bmp_data)
    print(f"Also saved as: {copy_name}")

if __name__ == "__main__":
    main()
