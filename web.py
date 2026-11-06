#!/usr/bin/env python3
"""
fpedal web frontend - zero dependencies (Python stdlib only).

Serves a single-page UI to program the FS3-P foot switch and streams
live key events from the pedal over Server-Sent Events.

Usage:
    ./web.py [--port 8321] [--host 127.0.0.1]

Runs the `fpedal` binary (built next to this file) for reads/writes,
and reads the pedal's keyboard event device directly for the live feed.
"""
import argparse
import json
import os
import queue
import subprocess
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
FPEDAL = os.path.join(HERE, "fpedal")
INDEX = os.path.join(HERE, "web", "index.html")

EVENT_DEV = "/dev/input/by-id/usb-1a86_e026-event-kbd"

# ---------- key names (subset of vendor/common.c key map) ----------
KEYS = [
    "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
    "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z",
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0",
    "enter", "esc", "backspace", "tab", "space",
    "-", "=", "[", "]", "\\", ";", "'", "`", ",", ".", "/",
    "capslock",
    "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12",
    "printscreen", "scrollock", "pause", "insert", "home", "pageup",
    "delete", "end", "pagedown", "left", "up", "right", "down",
    "XF86AudioMute", "XF86AudioRaiseVolume", "XF86AudioLowerVolume",
    "XF86AudioPause", "XF86AudioNext", "XF86AudioPrev",
]
MODS = ["l_ctrl", "l_shift", "l_alt", "l_win", "r_ctrl", "r_shift", "r_alt", "r_win"]
BUTTONS = ["mouse_left", "mouse_middle", "mouse_right"]

# libinput event code -> human name (for the live feed)
EV_NAMES = {
    1: "esc", 2: "1", 3: "2", 4: "3", 5: "4", 6: "5", 7: "6", 8: "7",
    9: "8", 10: "9", 11: "0", 12: "-", 13: "=", 14: "backspace",
    15: "tab", 16: "q", 17: "w", 18: "e", 19: "r", 20: "t", 21: "y",
    22: "u", 23: "i", 24: "o", 25: "p", 26: "[", 27: "]", 28: "\\",
    29: "#", 30: "a", 31: "s", 32: "d", 33: "f", 34: "g", 35: "h",
    36: "j", 37: "k", 38: "l", 39: ";", 40: "'", 41: "`", 42: "\\",
    43: "z", 44: "x", 45: "c", 46: "v", 47: "b", 48: "n", 49: "m",
    50: ",", 51: ".", 52: "/", 53: "capslock", 54: "f1", 55: "f2",
    56: "f3", 57: "f4", 58: "f5", 59: "f6", 60: "f7", 61: "f8",
    62: "f9", 63: "f10", 64: "f11", 65: "f12", 66: "printscreen",
    67: "scrollock", 68: "pause", 70: "insert", 71: "home", 72: "pageup",
    73: "delete", 74: "end", 75: "pagedown", 76: "left", 77: "up",
    78: "down", 79: "numlock", 85: "kp /", 86: "kp *", 87: "kp -",
    88: "kp +", 89: "enter", 91: "lshift", 92: "lctrl", 93: "lalt",
    94: "lwin", 95: "rshift", 96: "rctrl", 97: "ralt", 100: "rwin",
    104: "kp 7", 105: "kp 8", 106: "kp 9", 107: "kp 4", 108: "kp 5",
    109: "kp 6", 110: "kp 1", 111: "kp 2", 112: "kp 3", 113: "kp 0",
    114: "kp .", 115: "kp ,", 118: "kp enter", 119: "kp =",
    126: "f13", 127: "f14", 128: "f15", 129: "f16", 130: "f17",
    131: "f18", 132: "f19", 133: "f20", 134: "f21", 135: "f22",
    136: "f23", 137: "f24", 224: "space",
}


def run_fpedal(*args, timeout=10):
    """Run the fpedal binary, return (rc, stdout, stderr)."""
    try:
        p = subprocess.run(
            [FPEDAL, *args],
            capture_output=True, text=True, timeout=timeout,
        )
        return p.returncode, p.stdout, p.stderr
    except FileNotFoundError:
        return 127, "", "fpedal binary not found - run `make` first"
    except subprocess.TimeoutExpired:
        return 124, "", "fpedal timed out"


def get_state():
    rc, out, err = run_fpedal("show", "--json")
    if rc != 0:
        return None, err.strip()
    try:
        return json.loads(out), None
    except json.JSONDecodeError:
        return None, "fpedal returned invalid JSON:\n" + out


def apply_pedal(n, spec):
    rc, out, err = run_fpedal("set", str(n), spec, "--yes", "--json")
    if rc != 0 and not out.strip():
        return None, (err or "fpedal failed").strip()
    try:
        return json.loads(out), None
    except json.JSONDecodeError:
        return None, "fpedal returned invalid JSON:\n" + out


# ---------- live event feed ----------
class EventFeed:
    """Reads the pedal's keyboard event device and pushes events to SSE clients."""

    def __init__(self):
        self.clients = []
        self.clients_lock = threading.Lock()
        self.error = None
        self.fd = None
        self.thread = None

    def start(self):
        try:
            self.fd = os.open(EVENT_DEV, os.O_RDONLY)
        except OSError as e:
            self.error = f"cannot open {EVENT_DEV}: {e.strerror} (run with sudo or install the udev rule)"
            return
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self):
        import struct
        while True:
            try:
                buf = os.read(self.fd, 64)
            except OSError:
                break
            for off in range(0, len(buf) - struct.calcsize("llHHI"), 16):
                ts_sec, ts_usec, etype, code, val = struct.unpack_from(
                    "llHHI", buf, off)
                if etype != 2:  # EV_KEY
                    continue
                ev = {
                    "key": EV_NAMES.get(code, f"key{code}"),
                    "down": val == 1,
                    "t": time.time(),
                }
                with self.clients_lock:
                    dead = []
                    for q in self.clients:
                        try:
                            q.put_nowait(ev)
                        except Exception:
                            dead.append(q)
                    for q in dead:
                        self.clients.remove(q)

    def add_client(self):
        q = queue.Queue(maxsize=100)
        with self.clients_lock:
            self.clients.append(q)
        return q

    def drop_client(self, q):
        with self.clients_lock:
            if q in self.clients:
                self.clients.remove(q)


FEED = EventFeed()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass  # quiet

    def _send(self, code, body, ctype="application/json"):
        data = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def _json(self, obj, code=200):
        self._send(code, json.dumps(obj))

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        if url.path == "/":
            try:
                with open(INDEX, "rb") as f:
                    self._send(200, f.read(), "text/html; charset=utf-8")
            except OSError:
                self._send(500, "web/index.html missing", "text/plain")
        elif url.path == "/api/meta":
            self._json({"keys": KEYS, "mods": MODS, "buttons": BUTTONS,
                        "event_dev": EVENT_DEV,
                        "feed_error": FEED.error})
        elif url.path == "/api/state":
            state, err = get_state()
            if err:
                self._json({"error": err}, 500)
            else:
                self._json({"ok": True, "state": state})
        elif url.path == "/api/events":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "keep-alive")
            self.end_headers()
            if FEED.error:
                self.wfile.write(f"data: {json.dumps({'error': FEED.error})}\n\n".encode())
                return
            q = FEED.add_client()
            try:
                self.wfile.write(b"retry: 2000\n\n")
                while True:
                    try:
                        ev = q.get(timeout=15)
                        self.wfile.write(f"data: {json.dumps(ev)}\n\n".encode())
                    except queue.Empty:
                        self.wfile.write(b": keepalive\n\n")
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass
            finally:
                FEED.drop_client(q)
        else:
            self._json({"error": "not found"}, 404)

    def do_POST(self):
        url = urllib.parse.urlparse(self.path)
        if url.path == "/api/apply":
            length = int(self.headers.get("Content-Length", 0))
            try:
                body = json.loads(self.rfile.read(length) or b"{}")
            except json.JSONDecodeError:
                self._json({"error": "invalid JSON body"}, 400)
                return
            n = body.get("pedal")
            spec = body.get("spec")
            if not isinstance(n, int) or not 1 <= n <= 3:
                self._json({"error": "pedal must be 1, 2 or 3"}, 400)
                return
            if not isinstance(spec, str) or not spec.strip():
                self._json({"error": "spec is required"}, 400)
                return
            result, err = apply_pedal(n, spec.strip())
            if err:
                self._json({"error": err}, 500)
            else:
                self._json(result)
        else:
            self._json({"error": "not found"}, 404)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", type=int, default=8321)
    ap.add_argument("--host", default="127.0.0.1")
    args = ap.parse_args()

    FEED.start()
    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"fpedal web UI: http://{args.host}:{args.port}/")
    if FEED.error:
        print(f"warning: live feed unavailable: {FEED.error}")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
