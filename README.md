# fpedal — Linux programming tool for the FS3-P USB Triple Foot Switch

<img width=33% alt="614FwE2PUXL" src="https://github.com/user-attachments/assets/63cd2f85-3230-4aa8-9d20-9956ed14cb78" />

`fpedal` programs the **FS3-P USB Triple Foot Switch** (PCsensor / RDingtech,
USB ID `1a86:e026`) directly from Linux, replacing the Windows-only `.msi`
configurator that usually ships on a CD.

The pedal is a plain HID device — keyboard + mouse + gamepad interfaces —
plus a vendor configuration channel on the mouse interface. The mappings
you set are written to the pedal's own flash, so they persist across
reboots and work on any OS with no software running.

The protocol was reverse-engineered in
[rgerganov/footswitch](https://github.com/rgerganov/footswitch) (MIT);
`vendor/common.c` / `vendor/common.h` are vendored from that project.

## Identifying the hardware

    lsusb | grep -i qinheng
    # Bus 004 Device 056: ID 1a86:e026 QinHeng Electronics

(Yes, the FS3-P reports under the QinHeng vendor ID. Look for
`usb-1a86_e026-*` in `/dev/input/by-id/`.)

## Building

    sudo apt install libhidapi-dev   # or equivalent
    make

`make install` puts `fpedal` in `/usr/local/bin` and installs the udev rule
below.

## Usage

    fpedal show                  # read current mappings (read-only)
    fpedal set <1|2|3> <spec>    # program one pedal, other two preserved
    fpedal clear <1|2|3>         # reset one pedal to unconfigured
    fpedal listen [seconds]      # watch key events (verify programming)

Add `--yes` / `-y` to skip the confirmation prompt.

### Spec grammar

| spec | meaning |
|---|---|
| `a`, `f5`, `enter` | single key (full key map in `vendor/common.c`) |
| `ctrl+shift+a` | key combo; modifiers `ctrl shift alt win`, optional `l_`/`r_` prefix |
| `l_alt` | bare modifier (press/release Alt, no key) |
| `mouse_left` / `mouse_right` / `mouse_middle` | mouse button |
| `x:10 y:-5 w:2` | relative mouse motion, each in [-128, 127], combinable with a button |
| `ctrl+a mouse_left` | key **and** mouse button on one pedal |
| `str:hello world` | type a string (standalone, ≤ 38 chars) |
| `none` | unconfigure the pedal |

### Examples

    fpedal set 1 ctrl+shift+t      # open new tab
    fpedal set 2 l_win+l_ctrl      # modifier-only combo
    fpedal set 3 mouse_left        # click
    fpedal clear 3

## Safety

Bricking risk is low, but the protocol is undocumented, so `fpedal` is
careful by design:

- The pedal's flash only accepts a **full 3-pedal image**. A naive tool
  (including the original `footswitch` CLI) silently resets the pedals you
  don't mention to "unconfigured". `fpedal` reads all three pedals from the
  device first and rewrites the unmentioned ones byte-for-byte.
- If the read fails or returns an unknown type, it aborts **before writing**.
- It prints a `was` / `now` diff and asks for confirmation on a TTY.
- After writing it re-reads the pedal and warns if the read-back differs.

`show` and `listen` are strictly read-only.

## X event listener (xlisten)

`xlisten.py` is a native (no web stack) listener for the X events the pedal
produces. It listens to the pedal's own X slave devices — the XKB-mapped
keysyms, i.e. what the desktop actually receives — via `xinput test`,
resolves keycodes to key names with the current XKB mapping, and tracks
modifier state so combos print as e.g. `Ctrl+Win press`:

    python3 xlisten.py                 # auto-detects the pedal's X devices
    python3 xlisten.py --device 6      # or listen to a specific xinput device

It prints the pedal's firmware mapping (from `fpedal show`) up top so you
can compare what the pedal is programmed to send with what X receives.
Ctrl-C to quit. Requires `xinput` and `xmodmap` (xorg-x11-apps) and a
running X11 session.

## Graphical front end (`fpedal-ui`)

A native X11 app (Qt5, no web stack) that replaces the Windows configurator.
It is a thin GUI over the same `fpedal` CLI — every write still goes through
the safe read-modify-write + read-back path.

    sudo apt install qt6-base-dev   # or qtbase5-dev; needs libX11/libXtst too
    make gui
    ./fpedal-ui

One card per pedal:

- **capture combo…** — grabs the keyboard and reads raw X key events, so you
  physically press the combination you want (including Ctrl / Alt / Win, which
  a normal focused widget can't always see). It builds the spec for you
  (modifier-only combos work; `Esc` cancels; keys outside the pedal's keymap
  are rejected). Release the keys to finish.
- mouse buttons + relative motion (`x`/`y`/`wheel`) and a spec box you can
  type into directly for strings or hand-editing.
- **Apply** writes through `fpedal set` and shows the `was → now` result and
  the read-back verification; **Clear pedal** unconfigures one pedal.
- a live log at the bottom prints every X key / mouse event this window
  receives — focus the window and stomp a pedal to confirm what X actually
  gets.

Requires a running X11 session (the keyboard-capture feature uses the X
keyboard grab, so it is X11-only). Build a release against Qt6 by swapping
the `pkg-config` name `Qt5Widgets` → `Qt6Widgets` in the Makefile.

`make selftest` runs a non-interactive smoke test of the capture path (it
injects `Ctrl+Shift+T` with XTEST and checks the captured spec); it never
writes to the pedal.

## udev / permissions

`/dev/hidraw*` is root-only by default. Either run with `sudo`, or install
the rule (done by `make install`):

    sudo cp 50-fpedal.rules /etc/udev/rules.d/
    sudo udevadm control --reload-rules
    sudo udevadm trigger

## Protocol notes (for the curious)

Config channel: HID interface 1, 8-byte reports.

- Read pedal *n*: out `{01 82 08 n 00 00 00 00}` → 8-byte response.
  `resp[1]`: `0` unconfigured, `1`/`0x81` key, `2` mouse, `3` key+mouse,
  `4` string (payload starts at `resp[2]`, `resp[0]` = total length).
  `resp[2]` = modifiers, `resp[3]` = HID keycode, `resp[4]` = buttons,
  `resp[5..7]` = x/y/wheel.
- Write: out `{01 80 08 00 00 00 00 00}`, then for *n* = 1..3:
  `{01 81 08 n 00 00 00 00}` + the 8-byte data record
  (`data[0]` = length, `data[1]` = type).

## Files

- `fpedal.c` — the CLI tool (device I/O + `--json` for the GUI) (MIT)
- `gui/fpedal_ui.cpp` — native X11/Qt5 graphical front end (`fpedal-ui`)
- `scripts/gen_keymap.py` — build step: extracts the key map names into a
  header so the GUI validates keys against exactly what `fpedal` accepts
- `xlisten.py` — native X11 event listener for the pedal (xinput + xmodmap)
- `vendor/common.c`, `vendor/common.h` — HID key map & encoders, from
  rgerganov/footswitch (MIT, © Radoslav Gerganov, Daniel Manjarres)
- `50-fpedal.rules` — udev rule for passwordless access
- `Makefile`

## License

MIT (see `fpedal.c` and the vendored files).
