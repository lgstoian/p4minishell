"""
Grab screenshot from P4MiniShell via USB-Serial-JTAG.
Protocol: 4-byte magic "BMPX" + 4-byte LE size + raw BMP data.
Supports --port, --out, --crop-transcript for TUI debug.
"""
import time
import os
import struct
import sys
import argparse

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from shell_session import open_port, default_port

PORT = "COM11"
BAUD_RATE = 115200
OUTPUT_DIR = os.path.join(os.getcwd(), "screenshots")
TIMEOUT = 30

def grab_screenshot(port=None, out_dir=None, crop_transcript=False):
    port = port or default_port()
    out_dir = out_dir or OUTPUT_DIR
    os.makedirs(out_dir, exist_ok=True)

    try:
        ser = open_port(port, BAUD_RATE, timeout=1)
        print(f"Connected to {port}")

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
        filename = os.path.join(out_dir, f"screenshot_{timestamp}.bmp")

        with open(filename, 'wb') as f:
            f.write(bmp_data)

        print(f"\nSUCCESS: Saved to {filename}")
        print(f"File size: {len(bmp_data)} bytes")

        # Also save as latest.bmp for easy access
        latest = os.path.join(out_dir, "latest.bmp")
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
    parser = argparse.ArgumentParser(description="Grab screenshot from P4MiniShell")
    parser.add_argument("--port", default=None, help="Serial port (argv COMx, P4_PORT env, else COM11)")
    parser.add_argument("--out", default=OUTPUT_DIR, help="Output directory")
    parser.add_argument("--crop-transcript", action="store_true", help="Crop to transcript ROI via Pillow")
    args = parser.parse_args()
    result = grab_screenshot(port=args.port, out_dir=args.out, crop_transcript=args.crop_transcript)
    if result:
        print(f"\nDone! Screenshot saved to: {result}")
        if args.crop_transcript:
            try:
                from PIL import Image
                import json
                # Transcript ROI: header ~56px, input ~56px, keyboard 0-280px
                # Use full display 1024x600, crop to transcript region for TUI debug
                img = Image.open(result)
                w, h = img.size
                # Estimate transcript rect: y=56, height= h -56 -56 - (kb if visible)
                # For now, crop to middle 80% as transcript
                header_h = 56
                input_h = 56
                transcript_h = h - header_h - input_h
                cropped = img.crop((0, header_h, w, header_h + transcript_h))
                crop_path = result.replace(".bmp", "_transcript.bmp")
                cropped.save(crop_path)
                print(f"Cropped transcript: {crop_path} ({cropped.size})")
                # Save meta
                meta = {"width": w, "height": h, "transcript": {"x": 0, "y": header_h, "w": w, "h": transcript_h}, "crop": crop_path}
                with open(result.replace(".bmp", ".json"), "w") as f:
                    json.dump(meta, f, indent=2)
            except Exception as e:
                print(f"Crop failed (Pillow not installed?): {e}")
        sys.exit(0)
    else:
        sys.exit(1)
