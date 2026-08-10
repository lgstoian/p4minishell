"""
Grab screenshot from P4MiniShell via USB-Serial-JTAG.
Protocol: 4-byte magic "BMPX" + 4-byte LE size + raw BMP data.
"""
import serial
import time
import os
import struct
import sys

PORT = "COM11"
BAUD_RATE = 115200
OUTPUT_DIR = r"D:\p4minishell\screenshots"
TIMEOUT = 30

def grab_screenshot():
    os.makedirs(OUTPUT_DIR, exist_ok=True)

    try:
        ser = serial.Serial(PORT, BAUD_RATE, timeout=1)
        print(f"Connected to {PORT}")

        # Wait for boot
        time.sleep(2)

        # Clear buffer
        ser.reset_input_buffer()

        # Send screenshot command
        print("Sending screenshot command...")
        ser.write(b"screenshot\r\n")

        # Wait for the "streaming" message
        start = time.time()
        while time.time() - start < 10:
            if ser.in_waiting:
                line = ser.readline()
                if line:
                    text = line.decode('utf-8', errors='replace').strip()
                    if text:
                        print(f"  {text}")
                    if "streaming" in text.lower():
                        break
            time.sleep(0.01)

        # Now read the binary data
        # Protocol: 4-byte magic "BMPX" + 4-byte LE size + raw BMP data
        print("Reading binary data...")

        # Read magic bytes
        magic = ser.read(4)
        if magic != b'BMPX':
            print(f"ERROR: Invalid magic: {magic}")
            ser.close()
            return None

        # Read size (4 bytes, little-endian)
        size_bytes = ser.read(4)
        if len(size_bytes) != 4:
            print("ERROR: Could not read size")
            ser.close()
            return None

        data_size = struct.unpack('<I', size_bytes)[0]
        print(f"Expecting {data_size} bytes of BMP data")

        # Read the BMP data
        bmp_data = b''
        start = time.time()
        while len(bmp_data) < data_size and (time.time() - start) < TIMEOUT:
            remaining = data_size - len(bmp_data)
            chunk = ser.read(min(remaining, 4096))
            if chunk:
                bmp_data += chunk
            else:
                time.sleep(0.001)

        ser.close()

        print(f"Received {len(bmp_data)} bytes")

        if len(bmp_data) != data_size:
            print(f"ERROR: Size mismatch (got {len(bmp_data)}, expected {data_size})")
            return None

        # Verify BMP header
        if bmp_data[0:2] != b'BM':
            print(f"ERROR: Invalid BMP signature: {bmp_data[0:2]}")
            return None

        # Parse BMP header
        width = struct.unpack('<I', bmp_data[18:22])[0]
        height = struct.unpack('<I', bmp_data[22:26])[0]
        bpp = struct.unpack('<H', bmp_data[28:30])[0]

        print(f"BMP: {width}x{height} {bpp}-bit")

        # Save the file
        timestamp = time.strftime("%Y%m%d_%H%M%S")
        filename = os.path.join(OUTPUT_DIR, f"screenshot_{timestamp}.bmp")

        with open(filename, 'wb') as f:
            f.write(bmp_data)

        print(f"\nSUCCESS: Saved to {filename}")
        print(f"File size: {len(bmp_data)} bytes")

        # Also save as latest.bmp for easy access
        latest = os.path.join(OUTPUT_DIR, "latest.bmp")
        with open(latest, 'wb') as f:
            f.write(bmp_data)
        print(f"Also saved as: latest.bmp")

        return filename

    except Exception as e:
        print(f"Error: {e}")
        import traceback
        traceback.print_exc()
        return None

if __name__ == "__main__":
    result = grab_screenshot()
    if result:
        print(f"\nDone! Screenshot saved to: {result}")
        sys.exit(0)
    else:
        sys.exit(1)
