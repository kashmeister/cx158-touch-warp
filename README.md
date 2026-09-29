# cx158-touch-warp

A macOS launch agent that makes touches on a CX158 portable touch monitor land on the CX158,
whichever display the cursor is on.

## Hardware

CX158 is the model name the panel reports over EDID. It is likely a generic panel sold under
several brands; one example is the
[UPERFECT 14" OLED 3K portable monitor](https://www.amazon.com/dp/B0G48F6PVN).

To check a monitor, run `bin/cx158-touch-warp --check` and look for display vendor `0x0614`,
model `0x4778`, or run `hidutil list` and look for a touch controller with USB IDs
`0x416 0xc169`.

## Why

macOS drives the CX158's touch controller natively as an absolute-position mouse, but maps its
coordinates onto the display the cursor is currently on. With the cursor on another display, a
touch clicks there. The controller sends press and position in its first report, so moving the
cursor afterwards is too late to fix the click.

## How it works

An event tap intercepts mouse button events. A press counts as a touch when it:

- comes from hardware (source pid 0),
- is not from a trackpad (mouse subtype 0), and
- matches a touch-controller report of the same button from the last 60 ms.

A touch press is swallowed and re-posted, with its drags and release, at the matching point on the
CX158. All other input passes through unchanged. The device is never seized: if the agent stops,
touch behaves as it does natively.

The third check needs Input Monitoring, because a touchscreen and a USB mouse produce identical
events. Without that permission the agent remaps nothing.

## Requirements

- macOS 12 or later
- Xcode Command Line Tools
- A CX158 whose touch controller enumerates as USB `0416:c169` (for other monitors, see
  [Configuration](#configuration))

## Install

```sh
git clone https://github.com/kashmeister/cx158-touch-warp.git
cd cx158-touch-warp
./install.sh
```

Then enable `cx158-touch-warp` in System Settings → Privacy & Security under both
**Accessibility** and **Input Monitoring**. If it is not listed, add `bin/cx158-touch-warp` with
**+**.

Accessibility takes effect within 10 s. Input Monitoring applies only to new processes: the agent
relaunches itself once, 30 s after starting. After that, restart it by hand:

```sh
launchctl kickstart -k gui/$(id -u)/local.cx158-touch-warp
```

The agent is ready when `~/Library/Logs/cx158-touch-warp.log` shows:

```
Input Monitoring granted; HID manager open
Accessibility granted; event tap active
```

## Usage

| Command | Effect |
|---|---|
| `./install.sh --verbose` | Reinstall with per-event logging. `./install.sh` turns it off. |
| `bin/cx158-touch-warp --check` | Print displays, device and permission status, then exit. |
| `launchctl kickstart -k gui/$(id -u)/local.cx158-touch-warp` | Restart the agent. |
| `tail -f ~/Library/Logs/cx158-touch-warp.log` | Follow the log. |
| `./uninstall.sh` | Stop and remove the agent. |

The agent posts a notification when touch stops working (digitizer mode, below), when it recovers,
and when a permission is missing.

## Troubleshooting

**Touch does nothing.** On some power-ups the controller enumerates as a digitizer (USB
`eeef:2828`) that macOS cannot drive. The agent posts a notification and logs a warning. Remove
power from the monitor for about 15 seconds; reconnecting only the data cable is not enough.

**Touches still land on the wrong display.** The log names the missing permission. Both are needed.

**A permission is enabled but the log says it is missing.** Privacy & Security can list several
`cx158-touch-warp` entries from earlier builds, and only the current one counts. Remove them all
with **−**, then add `bin/cx158-touch-warp` once.

**`--check` and the log disagree about permissions.** Run from a terminal, `--check` reports the
terminal's permissions. The log is authoritative.

## Configuration

Constants at the top of `cx158-touch-warp.c`. Run `./install.sh` after changing them.

| Constant | Default | Meaning |
|---|---|---|
| `DISPLAY_VENDOR`, `DISPLAY_MODEL` | `0x0614`, `0x4778` | CoreGraphics vendor and model of the target display |
| `TOUCH_VID`, `TOUCH_PID` | `0x0416`, `0xc169` | USB IDs of the touch controller in mouse mode |
| `DIGI_VID`, `DIGI_PID` | `0xeeef`, `0x2828` | USB IDs of the controller in digitizer mode |
| `CORRELATION_WINDOW` | 0.06 s | Maximum age of the touch report behind a press |

Other monitors work if their touch controller is a HID mouse with absolute X/Y. `--check` lists
each display's vendor and model; `hidutil list` shows USB IDs.

## Uninstall

```sh
./uninstall.sh
```

Then remove the `cx158-touch-warp` entries from Accessibility and Input Monitoring.

## License

MIT
