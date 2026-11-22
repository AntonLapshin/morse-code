# Hardware notes — IdeaSpark ESP32 + 1.9″ ST7789

This app targets the IdeaSpark ESP32-WROOM board with the on-board 1.9″
170×320 ST7789 LCD. There is no display wiring to do: the panel is soldered to
fixed GPIOs, and the board exposes two push buttons.

## Display

| Signal | GPIO | Notes |
|---|---|---|
| MOSI (SDA) | 23 | SPI data |
| SCLK (SCL) | 18 | SPI clock |
| CS | 15 | chip select |
| DC | 2 | data/command |
| RST | 4 | reset |
| BL | 32 | backlight, PWM |

Initialisation in `firmware/src/main.cpp`:

```cpp
SPI.begin(18, -1, 23, 15);     // SCLK, MISO, MOSI, CS
tft.bringUp();                 // no RST pulse, no SWRESET
```

`bringUp()` sends `SLPOUT`, `COLMOD` (16-bit), `MADCTL` for rotation 2,
`INVON`, `NORON` and `DISPON`, then sets the 170×320 geometry. It
deliberately skips the hardware reset pulse and `SWRESET`: the panel
controller keeps its GRAM powered by the 3.3 V rail across an ESP32 reset, so
the previous frame stays visible while the chip reboots. On a true power-on
the panel has already reset itself, so the same command sequence is sufficient.

The UI is composed in a `GFXcanvas16` in RAM and pushed to the panel with one
`writePixels()` call per refresh (partial strips for the pattern/result rows, a
full frame otherwise). The physical screen is never filled first, which removes
the black flash that a clear-then-draw refresh would cause.

The backlight (GPIO32) is switched on at the very start of `setup()`, before
serial or display initialisation, so a `RESET`/`EN` press does not leave the
screen dark for the whole boot. `DISPLAY_ROTATION` is `2` in `config.h`; use `0`
if the image is upside-down on your revision.

## Buttons

### BOOT — the Morse key

| Property | Value |
|---|---|
| GPIO | 0 |
| Active level | LOW (pressed) |
| Pull-up | internal (`INPUT_PULLUP`) |
| Functions | tap = dot, hold = dash, ≥1.5 s hold = action menu (`SEND` / `CLEAR` / `READ n` / `CLOSE`) |

GPIO0 is also the ESP32 **boot-strap** pin. The board's USB-serial auto-reset
circuit uses it to enter the ROM bootloader while flashing. Consequences:

- Do not hold BOOT while pressing RESET or starting an upload, or the chip will
  stay in download mode.
- Opening a serial monitor or resetting through a tool can pulse GPIO0 briefly;
  the firmware may decode that as a dot. This only happens around flashing /
  port changes and is harmless.

### RESET — the left-hand button

The second physical button is wired to **EN**, the ESP32 hardware reset. It is
not connected to a GPIO, so the firmware cannot distinguish it from any other
reset. Pressing it reboots the board, which clears the text buffer.

The firmware keeps the reset visually cheap: the backlight is turned on as the
first action in `setup()`, the panel is re-initialised without a reset pulse or
`SWRESET` (so its GRAM and the last frame survive), and the new frame is
blitted in one write. Measured on the reference board, the first frame is ready
about 370 ms after the reset pulse, of which the ESP32 boot ROM accounts for
roughly 260 ms.

This is why the app offers two software-only clear gestures:

1. **Hold BOOT for at least 1.5 s** (the pattern row shows `MENU`) to open
   the action menu, then pick `CLEAR` inside it.
2. **Optional dedicated button** — see below.

### Optional clear button

To get a dedicated "clear" button that does not reboot, wire a momentary
switch between GND and a free GPIO, then set it in `firmware/src/config.h`:

```cpp
#define BTN_CLEAR_PIN 27   // -1 = disabled
```

```
GPIO27 ────o  o──── GND
```

Use a pin with an internal pull-up (27, 26, 25, 33, …). GPIO34–39 are
input-only and have **no** internal pull-up, so they need an external 10 kΩ
resistor to 3.3 V. The firmware enables `INPUT_PULLUP` and triggers the clear on
release.

## Timing model

Morse input is duration-based, not edge-based:

```
press ──┬─────────────────────────────── release
        │← <220 ms → dot
        │← 220 ms … 1.5 s → dash
        └← ≥1.5 s → action menu (commits any pending letter first)
```

After the last symbol:

```
release ── 700 ms silence ── commit letter (or next press commits early)
         └─ 4000 ms silence ── insert a word space
```

The commit countdown bar under the pattern row visualises the 700 ms window.
Constants live in `firmware/src/config.h`; the full table is in the
[README](../README.md#timing-and-configuration).

## USB / serial

- USB-serial chip: **CH340**, USB-C.
- Linux driver: `ch341` (built into most distros). The port is usually
  `/dev/ttyUSB0`.
- Baud rate: **115200** for both the boot log and flashing.
- Linux permissions: the device is typically owned by `root:uucp` (or
  `root:dialout`). Add your user to the matching group and re-login:

  ```bash
  ls -l /dev/ttyUSB0              # note the group
  sudo usermod -aG uucp "$USER"   # or dialout
  ```

- The board auto-enters download mode for uploads. If it does not, hold BOOT,
  tap RESET, release BOOT, and start the upload.

## Power and memory

The app uses WiFi (TLS to the Cloudflare Worker) and hand-rolled JSON, so
the image is larger than a bare trainer build — the WiFi/TLS stack plus
the 108 KB display canvas in RAM are the bulk of it. Powering the board
from any USB port is enough; the backlight at PWM 160 is the largest
consumer.
