#!/usr/bin/env python3
"""display_glitch_watch.py - observe the physical panel for the "BSOD".

The "BSOD" is a full-screen light-blue flash caused by a MIPI-DSI bridge
underrun. It is a *panel-level* event: the LVGL framebuffer and the serial
console are always correct, so `screenshot`/`bsod_watch.py` cannot see it. This
tool watches the panel with a webcam and timestamps every flash, optionally
recording the serial console in parallel for context.

Usage:
  python tools/display_glitch_watch.py --duration 120 --reset
  python tools/display_glitch_watch.py --device lorgar --preview
  python tools/display_glitch_watch.py --calibrate --duration 10

Camera selection: --device <substring> matched against DirectShow names via
pygrabber, else --index N, else the first camera that opens. --preview shows
the frame with the detection ROI so the crop can be confirmed/tuned.
"""

import argparse
import os
import sys
import threading
import time

import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    from shell_session import open_port, default_port, hard_reset
except Exception:  # serial is optional; the camera metric stands alone
    open_port = default_port = hard_reset = None

# Default ROI over the panel in the Lorgar's 1280x720 frame (x0,y0,x1,y1).
DEFAULT_ROI = (400, 25, 875, 700)
# Panel blue dominance (mean B - mean R over the ROI). Normal UI ~23 (tight);
# a DSI underrun flash jumps to ~45-65. Measured on the Lorgar at 640x480.
DEFAULT_THRESHOLD = 45.0
OUT_DIR = os.path.join(os.getcwd(), "screenshots")


def resolve_camera_index(device, index):
    if index is not None:
        return index
    if device:
        try:
            from pygrabber.dshow_graph import FilterGraph
            for i, name in enumerate(FilterGraph().get_input_devices()):
                if device.lower() in name.lower():
                    return i
        except Exception as exc:
            print("pygrabber lookup failed (%s); probing indices" % exc)
    # Probe the first index that yields a frame.
    for i in range(6):
        cap = cv2.VideoCapture(i, cv2.CAP_DSHOW)
        if cap.isOpened():
            ok, _ = cap.read()
            cap.release()
            if ok:
                return i
        else:
            cap.release()
    return 0


def panel_blue_metric(frame, roi):
    """Return (blue_dominance, light_blue_fraction) for the panel ROI.

    blue_dominance = mean(B) - mean(R). The panel's normal UI holds a tight
    ~23 (bright white keys pull R up); a DSI underrun washes the panel blue and
    the metric jumps to 45-65. Light-blue fraction is reported for context.
    """
    x0, y0, x1, y1 = roi
    sub = frame[y0:y1, x0:x1].astype(np.int16)
    if sub.size == 0:
        return 0.0, 0.0
    b = sub[:, :, 0]
    g = sub[:, :, 1]
    r = sub[:, :, 2]
    blue_dominance = float((b - r).mean())
    light_blue = float(((b > 140) & (g > 140) & (r > 90) & (r < 225) & (b >= r - 10)).mean())
    return blue_dominance, light_blue


class SerialTail(threading.Thread):
    """Read the board console in the background; keep the last lines."""

    def __init__(self, port):
        super().__init__(daemon=True)
        self.ser = None
        self.lines = []
        self.uptime0 = time.time()
        self._stop = False
        if port and open_port is not None:
            try:
                self.ser = open_port(port, 115200, timeout=0.2)
            except Exception as exc:
                print("serial open failed (%s); camera-only" % exc)

    def run(self):
        buf = b""
        while not self._stop and self.ser is not None:
            try:
                chunk = self.ser.read(4096)
            except Exception:
                break
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", "replace").strip("\r")
                if text:
                    self.lines.append((time.time(), text))
                    if len(self.lines) > 400:
                        del self.lines[:200]

    def tail(self, n=4):
        return [line for _t, line in self.lines[-n:]]

    def since(self, start_time):
        return [(t, line) for t, line in self.lines if t >= start_time]

    def write(self, line):
        if self.ser is None:
            return
        try:
            self.ser.write((line + "\r\n").encode())
        except Exception as exc:
            print("serial write failed (%s)" % exc)

    def stop(self):
        self._stop = True
        if self.ser is not None:
            try:
                self.ser.close()
            except Exception:
                pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--device", default="lorgar", help="camera name substring")
    ap.add_argument("--index", type=int, default=None, help="camera index override")
    ap.add_argument("--port", default=None, help="serial port (default)")
    ap.add_argument("--duration", type=float, default=120.0, help="seconds to watch")
    ap.add_argument("--roi", default=None, help="x0,y0,x1,y1 panel crop")
    ap.add_argument("--threshold", type=float, default=DEFAULT_THRESHOLD,
                    help="blue-dominance threshold (mean B - mean R); default 45")
    ap.add_argument("--out", default=OUT_DIR)
    ap.add_argument("--reset", action="store_true", help="hard-reset the board first")
    ap.add_argument("--preview", action="store_true", help="show ROI and exit")
    ap.add_argument("--calibrate", action="store_true", help="print the fraction and exit")
    ap.add_argument("--stress", action="store_true",
                    help="send `display stress on` after boot and off at the end")
    args = ap.parse_args()

    roi = DEFAULT_ROI
    if args.roi:
        roi = tuple(int(v) for v in args.roi.split(","))

    idx = resolve_camera_index(args.device, args.index)
    cap = cv2.VideoCapture(idx, cv2.CAP_DSHOW)
    if not cap.isOpened():
        print("camera %d did not open" % idx)
        return 1
    # MJPG at 640x480 sustains ~30 fps (720p drops to ~10 fps, too slow to
    # catch a short flash). A one-frame panel glitch is a full-screen field,
    # so the low resolution does not hurt detection.
    cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*"MJPG"))
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)
    cap.set(cv2.CAP_PROP_FPS, 60)
    for _ in range(10):
        cap.read()
    fw = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    fh = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    # DEFAULT_ROI is expressed on the 1280x720 frame; scale to the real size.
    roi = (int(roi[0] * fw / 1280), int(roi[1] * fh / 720),
           int(roi[2] * fw / 1280), int(roi[3] * fh / 720))

    if args.preview:
        frame = cap.read()[1]
        cv2.rectangle(frame, (roi[0], roi[1]), (roi[2], roi[3]), (0, 0, 255), 3)
        cv2.imwrite(os.path.join(args.out, "glitch_preview.png"), frame)
        print("wrote%sglicht_preview.png (camera %d)" % (os.sep, idx))
        cap.release()
        return 0

    if args.calibrate:
        t0 = time.time()
        while time.time() - t0 < args.duration:
            blue, lb = panel_blue_metric(cap.read()[1], roi)
            print("blue_dominance=%.1f light_blue=%.3f" % (blue, lb))
            time.sleep(0.2)
        cap.release()
        return 0

    os.makedirs(args.out, exist_ok=True)

    if args.reset and hard_reset is not None:
        port = args.port or (default_port() if default_port else None)
        if port:
            print("hard resetting %s" % port)
            hard_reset(port)

    tail = SerialTail(args.port or (default_port() if default_port else None))
    tail.start()
    if args.stress:
        time.sleep(3.0)   # let the boot script settle
        tail.write("display stress on")
        print("sent: display stress on")

    print("watching camera %d for %.0fs (roi=%s threshold=%.2f)..." %
          (idx, args.duration, roi, args.threshold))
    t0 = time.time()
    in_glitch = False
    glitch_start = 0.0
    prev_start = None
    peak_blue = 0.0
    peak_lb = 0.0
    peak_frame = None
    events = []
    frames = 0
    peak_blue_overall = 0.0
    cap_t0 = time.time()
    while time.time() - t0 < args.duration:
        ok, frame = cap.read()
        if not ok:
            continue
        frames += 1
        blue, lb = panel_blue_metric(frame, roi)
        now = time.time()
        if blue > peak_blue_overall:
            peak_blue_overall = blue
        if blue >= args.threshold:
            if not in_glitch:
                in_glitch = True
                glitch_start = now
                peak_blue = blue
                peak_lb = lb
                peak_frame = frame.copy()
            elif blue > peak_blue:
                peak_blue = blue
                peak_lb = lb
                peak_frame = frame.copy()
        elif in_glitch:
            in_glitch = False
            events.append((glitch_start, now))
            rel = glitch_start - t0
            gap = (glitch_start - prev_start) if prev_start is not None else 0.0
            prev_start = glitch_start
            stamp = time.strftime("%H:%M:%S", time.localtime(glitch_start))
            path = os.path.join(
                args.out,
                "glitch_%s_%.1f.jpg" % (time.strftime("%Y%m%d_%H%M%S",
                                                      time.localtime(glitch_start)), rel))
            if peak_frame is not None:
                cv2.imwrite(path, peak_frame)
            print("#%d  t=%.1fs wall=%s  dur=%.2fs  interval=%.1fs  peak_blue=%.1f peak_lb=%.2f  -> %s"
                  % (len(events), rel, stamp, now - glitch_start, gap, peak_blue, peak_lb, path))
            for ts, line in tail.since(glitch_start - 1.5):
                print("    | +%.2f  %s" % (ts - glitch_start, line[:150]))
        # opportunistic key to stop early
        if cv2.waitKey(1) & 0xFF == 27:
            break
    cap.release()
    if args.stress:
        tail.write("display stress off")
    tail.stop()
    fps = frames / max(0.001, time.time() - cap_t0)
    print("done: %d events in %.0fs (camera %.1f fps, peak_blue=%.1f)"
          % (len(events), args.duration, fps, peak_blue_overall))
    return 0


if __name__ == "__main__":
    sys.exit(main())
