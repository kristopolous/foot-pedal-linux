#!/usr/bin/env python3
"""
xlisten - native X11 event listener for the FS3-P foot switch.

Listens to the X events produced by the pedal's own X slave devices
(via `xinput test <device>`), resolves keycodes to keysym names with the
current XKB mapping, and tracks modifier state so combos print as e.g.
"Ctrl+Win press". The pedal's firmware mapping (fpedal show) is shown up
top for comparison.

No web stack, no dependencies beyond xinput, xmodmap, and fpedal.

    ./xlisten            # run until Ctrl-C
"""
import json
import os
import re
import subprocess
import sys
import threading
import time
from datetime import datetime

HERE = os.path.dirname(os.path.abspath(__file__))
FPEDAL = os.path.join(HERE, "fpedal")

# keysyms that count as modifiers when building combo labels
MOD_KEYS = {
    "Control_L": "Ctrl", "Control_R": "Ctrl",
    "Shift_L": "Shift", "Shift_R": "Shift",
    "Alt_L": "Alt", "Alt_R": "Alt",
    "Super_L": "Win", "Super_R": "Win",
    "Meta_L": "Meta", "Meta_R": "Meta",
}
MOD_ORDER = ["Ctrl", "Shift", "Alt", "Win", "Meta"]

KEY_RE = re.compile(r"^key (press|release)\s+(\d+)\s*$")
BTN_RE = re.compile(r"^button (press|release)\s+(\d+)\s*$")


def clean_device_label(line):
    """Strip xinput tree characters and the id= suffix from a device line."""
    label = line.split("id=")[0]
    label = re.sub(r"[\u2500-\u257f\u2390-\u23bb\u21b3]+", " ", label)
    return re.sub(r"\s+", " ", label).strip()


def find_devices():
    """Return {role: (id, name)} for the pedal's X slave devices."""
    out = subprocess.run(["xinput", "list"], capture_output=True, text=True).stdout
    dev = {}
    for line in out.splitlines():
        if "1a86:e026" not in line:
            continue
        m = re.search(r"id=(\d+)", line)
        if not m:
            continue
        did = int(m.group(1))
        name = clean_device_label(line)
        if "Keyboard" in line:
            dev["kbd"] = (did, name)
        elif "Mouse" in line:
            dev["mouse"] = (did, name)
    return dev


def keycode_names():
    """keycode -> keysym name, from the current XKB mapping."""
    names = {}
    try:
        out = subprocess.run(["xmodmap", "-pke"], capture_output=True, text=True).stdout
        for line in out.splitlines():
            m = re.match(r"keycode\s+(\d+)\s*=\s*(\S+)", line)
            if m:
                names[int(m.group(1))] = m.group(2)
    except FileNotFoundError:
        pass
    return names


def firmware_mapping():
    try:
        p = subprocess.run([FPEDAL, "show", "--json"],
                           capture_output=True, text=True, timeout=10)
        state = json.loads(p.stdout)
        parts = []
        for ped in state.get("pedals", []):
            bits = list(ped.get("mods") or [])
            if ped.get("key"):
                bits.append(ped["key"])
            if ped.get("button"):
                bits.append(ped["button"])
            s = "+".join(bits) if bits else "unconfigured"
            parts.append(f"pedal {ped['n']}: {s}")
        return "   ".join(parts)
    except Exception:
        return "(fpedal not available or pedal not connected)"


class DeviceListener(threading.Thread):
    """Streams `xinput test <id>` and prints resolved events."""

    def __init__(self, did, name, keymap):
        super().__init__(daemon=True)
        self.did = did
        self.name = name
        self.keymap = keymap
        self.mods = set()
        self.lock = threading.Lock()
        self.proc = None

    def run(self):
        try:
            self.proc = subprocess.Popen(
                ["xinput", "test", str(self.did)],
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                text=True, bufsize=1,
            )
        except FileNotFoundError:
            print("xinput not found", file=sys.stderr)
            return
        for line in self.proc.stdout:
            m = KEY_RE.match(line.strip())
            if m:
                self.on_key(m.group(1), int(m.group(2)))
                continue
            m = BTN_RE.match(line.strip())
            if m:
                btn = {"1": "left", "2": "middle", "3": "right"}.get(m.group(2), m.group(2))
                self.print_ev(f"button {btn} {m.group(1)}")
                continue
            # ignore motion, device changes, etc.

    def on_key(self, kind, code):
        name = self.keymap.get(code, f"keycode:{code}")
        short = MOD_KEYS.get(name)
        with self.lock:
            if short:
                if kind == "press":
                    self.mods.add(short)
                else:
                    self.mods.discard(short)
                combo = "+".join(x for x in MOD_ORDER if x in self.mods)
                label = f"{combo} (modifier)" if combo else "(modifier)"
            else:
                combo = "+".join(x for x in MOD_ORDER if x in self.mods)
                label = f"{combo}+{name}" if combo else name
        self.print_ev(f"{kind}  {label}")

    def print_ev(self, desc):
        ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        print(f"  [{ts}] {desc}", flush=True)

    def stop(self):
        if self.proc:
            self.proc.kill()


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--device", type=int, help="listen to a specific xinput device id instead of auto-detecting the pedal")
    args = ap.parse_args()

    if args.device is not None:
        out = subprocess.run(["xinput", "list"], capture_output=True, text=True).stdout
        line = next((l for l in out.splitlines()
                     if re.search(r"id=" + str(args.device) + r"\b", l)), None)
        if not line:
            print(f"no xinput device with id {args.device}")
            sys.exit(1)
        name = clean_device_label(line)
        devs = {"custom": (args.device, name)}
    else:
        devs = find_devices()
        if "kbd" not in devs:
            print("pedal keyboard device not found in X.")
            print("expected a device named 'HID 1a86:e026 Keyboard' (xinput list)")
            sys.exit(1)

    print("FS3-P foot switch - X event listener")
    print(f"firmware: {firmware_mapping()}")
    for role, (did, name) in sorted(devs.items()):
        print(f"device:   {name}  (id {did})")
    print("listening... stomp a pedal, Ctrl-C to quit")
    print()

    keymap = keycode_names()
    listeners = [DeviceListener(did, name, keymap) for _, (did, name) in devs.items()]
    for l in listeners:
        l.start()

    try:
        while True:
            time.sleep(0.5)
    except KeyboardInterrupt:
        print("\nstopped.")
    finally:
        for l in listeners:
            l.stop()


if __name__ == "__main__":
    main()
