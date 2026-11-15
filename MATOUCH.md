# MaTouch Knob: setup, backup, flash, calibrate

Notes for running Scott Bezek's SmartKnob firmware on the Makerfabs MaTouch Knob
(ESP32-S3, 3205B motor, MT6701 sensor, GC9A01 round screen). Build target: `matouch`.

## 1. Install the tools (once)

1. Install **VS Code** (code.visualstudio.com).
2. In VS Code: Extensions (the four-squares icon) → search **PlatformIO IDE** → Install. Restart VS Code when asked.
3. File → Open Folder → `Desktop/smart-knob`. PlatformIO notices `platformio.ini` and sets up the project.
4. Open a PlatformIO terminal: the PlatformIO ant-head icon in the left bar → Quick Access →
   Miscellaneous → **New Terminal**. All commands below go in that terminal (it knows where `pio` is).

## 2. First build (no knob needed)

```
pio run -e matouch
```

The first run downloads the ESP32 compiler and libraries (a few minutes, several hundred MB).
This build has never been compiled yet, so expect errors. Copy the error text to Claude, fix one thing, repeat.

## 3. Back up the factory firmware (before the first upload)

Plug the knob in with USB-C, then find its port:

```
ls /dev/cu.usbmodem*
```

If nothing shows up: unplug, hold the **Flash** button on the back, plug in, release. That forces
"download mode" (the chip waits to be programmed instead of running its program).

Copy the whole 16 MB of flash memory to a file outside the repo (replace the port name with yours):

```
mkdir -p ~/smart-knob-backups
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --port /dev/cu.usbmodemXXXX \
  read_flash 0 0x1000000 ~/smart-knob-backups/matouch-factory.bin
```

Takes a few minutes. To put the factory firmware back later:

```
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --port /dev/cu.usbmodemXXXX \
  write_flash 0 ~/smart-knob-backups/matouch-factory.bin
```

## 4. Upload and watch

```
pio run -e matouch -t upload
pio device monitor -e matouch
```

The monitor shows the knob's log messages and sends your keypresses to it. Quit with Ctrl+C.
If the upload can't connect, use the Flash-button trick from step 3, then press Reset after uploading.

## 5. Calibrate (once, saved on the knob)

1. Set the knob down and **don't touch it**.
2. In the monitor, press **C**. The motor turns back and forth by itself while it measures:
   - which direction the motor turns relative to the sensor
   - pole pairs (should come out as **7**)
   - the "zero offset" (where the motor's magnets sit relative to the sensor's zero)
3. It saves the result to the knob's flash memory. You only redo this if you swap the motor or sensor.
4. Press **Space** to step through Scott's haptic demos (detents, end stops, return-to-center...).

## What to check, and the likely fix

| Symptom | Likely cause | Where to fix |
|---|---|---|
| Screen stays black | Backlight or screen pins | `PIN_LCD_BACKLIGHT`, `TFT_*` in `[env:matouch]` |
| Colors wrong (negative-looking) or image mirrored | Screen color inversion / rotation | `TFT_INVERSION_ON`/`OFF` flag, `SK_DISPLAY_ROTATION` |
| Sensor error during calibration, angle jumps | MT6701 read settings differ from Scott's board | `firmware/src/mt6701_sensor.cpp` (SPI mode/speed) |
| Motor buzzes or stutters during calibration | Motor pin order | swap two of `PIN_MOTOR_A/B/C` |
| Turning right counts down | Sensor direction | flip `SK_INVERT_ROTATION` (0/1) |
| Knob shuts off when unplugged from USB | Power latch | `PIN_POWER_HOLD` (and the code in `main.cpp`) |
| Clicks feel weak, mushy or buzzy | Motor tuning | `firmware/src/motors/matouch_3205b.h` (P, D) |

## What's MaTouch-specific in this branch

- `platformio.ini`: the `[env:matouch]` block (pins and features)
- `firmware/src/motors/matouch_3205b.h` + `motor_config.h`: motor tuning, values from Makerfabs' firmware
- `firmware/src/motor_task.h`: 3-signal motor driver when `SK_MOTOR_3PWM=1`
- `firmware/src/main.cpp`: power latch at startup when `PIN_POWER_HOLD` is set

- `firmware/src/cst816s_touch.*`: reads the touch chip (one finger, I2C address 0x15, pins 6/8)
- `firmware/src/touch_gestures.*`: turns touches into gestures; tuning values at the top of `touch_gestures.h`
- `firmware/src/touch_ring.*`: measures sliding around the rim; tuning at the top of `touch_ring.h`
- `firmware/src/ring_stepper.*`: turns rim travel into value steps (TRACK mode)
- `firmware/src/motor_task.*`: `setFollow()` and the follow controller (FOLLOW mode)
- `firmware/src/interface_task.*`: what each gesture does (`updateTouch()`), when `SK_TOUCH=1`

## Touch gestures

Every gesture is printed in the monitor, e.g. `Touch: DOUBLE_TAP at (120, 98)`, so the monitor doubles as a touch test mode.

| Gesture | Where | Does |
|---|---|---|
| Function button press | in an app | main menu (apps, Settings, Sleep, Power off) |
| Function button press | in a menu | back to the app |
| Function button hold (2 s) | anywhere | power off |
| Swipe left / right | in an app | next / previous page (e.g. Volume -> Scroll) |
| Tap, double tap, triple tap, long press, swipe up / down | in an app | free for app actions (tap = knob push) |
| Turn the knob | in a menu | move through the list (one click per item) |
| Tap / swipe left | in a menu | open / choose |
| Swipe right | in a menu | back |
| Slide around the rim | anywhere | acts like turning the knob (Settings > Touch) |

Single and double taps wait ~0.25 s to make sure another tap isn't coming.

Rim slider: a touch in the outer band of the screen (more than 75 px from center) becomes a slide
after ~4 degrees of travel; less than that still counts as a tap. Clockwise raises the value.

- **Track** (default): the value changes exactly as if you'd turned the knob the same angle, using the
  current mode's detent spacing, limits and clicks (`ring_stepper.*`).
- **Follow**: the motor turns the knob to follow your finger. Tuning: `FOLLOW_P`, `FOLLOW_D`,
  `FOLLOW_LIMIT` at the top of `firmware/src/motor_task.cpp`.

## Apps and settings

- **Apps:** Mac (Volume, Scroll, Arrows) and Demo (Scott's haptic behaviors). Home = Mac.
- **Settings:** Brightness (10-100%), Click strength (20-200%, felt as you turn), Touch (rim: Track /
  Follow / Off), Auto off, Bluetooth, Calibrate, Battery, Version.
- **Bluetooth:** Connected / Not connected (tap: paired-device count), Add device (pairing open for
  2 minutes, with a countdown), Forget all. Outside pairing only already-paired devices can connect
  (`PAIRED_ONLY` in `ble_output.cpp`; set false if a paired Mac ever can't reconnect). With nothing
  paired, pairing stays open until the first device pairs.
- Lists slide with the knob between clicks and snap to each item. In Settings the position ring runs
  along the bottom (first item on the left) and the status line (Bluetooth, charging, battery) shows at the top.
- Pop-ups: Bluetooth connected / disconnected / paired / pairing closed, Charging / On battery,
  Battery low (15%, again after charging past 25%).
- Control screens show the app name at the top.
- Fine volume holds Shift+Option down for a whole turn (released 0.6 s after you stop), so apps don't
  see rapid Option taps (e.g. Claude's "double-tap Option" shortcut).
- Saved on the knob (ESP32 Preferences): brightness, haptics, touch mode, Volume level, last app.
- Code: navigation section at the end of `interface_task.cpp`; layouts in `display_task.cpp`
  (`UiScreen` in `ui_screen.h`); storage in `settings_store.*`; version in `version.h`.

## Bluetooth (Mac / iPad)

The knob pairs as **SmartKnob** (System Settings → Bluetooth). It acts as a keyboard + media keys + mouse wheel;
nothing to install on the Mac. The monitor prints `Bluetooth: connected` / `disconnected`.

The first three modes send keys whenever the value changes (knob, rim TRACK or FOLLOW):

| Mode | Sends | Feel |
|---|---|---|
| Volume | volume up/down in quarter steps (Shift+Option) | 0-64 with hard stops, 5 degree clicks; push into an end stop to line up with the Mac |
| Scroll | mouse wheel, clockwise = down | spring-return: turn further = scroll faster + more resistance; rim scrolls directly |
| Arrows | right / left arrow keys | 15 degree firm clicks |

Code: `firmware/src/ble_output.*` (keys), `updateBleOutput()` in `interface_task.cpp` (which mode sends what).
Library: ESP32-BLE-CompositeHID v0.4.0 (NimBLE).

## Power and battery

- **On (battery):** hold the Function button 1-2 s until the screen lights, then let go.
- **Plugging in a charger while it's off** wakes it to a **charging screen** (battery %, or "Charged").
  Tap the screen to turn it off / on. It doesn't hold the power on, so unplugging switches it back off.
  Press the Function button to start
  normally. Works with wall chargers too: it's the power arriving that wakes it (`main.cpp` checks
  for a fresh power-up with the button not held).
- **Off:** hold the Function button 2 s, then let go. On USB the knob can't switch itself off
  (USB powers it directly); it says so in the monitor and carries on.
- **Battery:** read every 5 s and smoothed; logged every 30 s. On battery the shown level only goes
  down; it can rise only while plugged in. Still a rough guide: it reads high while charging, sags
  under motor load, and means little with no battery fitted.
- **USB / charging:** detected when plugged into a computer (the USB port's 1 ms "frame" signal keeps
  counting). A plain wall charger can't be detected, and a sleeping Mac may stop the signal.

- **Sleep:** after 30 s idle the screen dims; after 2 min the screen turns off and the motor relaxes
  (knob turns freely, no clicks). Any touch, turn or button press wakes it; the waking touch/turn does
  nothing else. **Auto off** (Settings, default 15 min; 5 / 15 / 30 min / Never) fully powers off when
  idle on battery. Not while plugged into a computer. Times: `SLEEP_*` in `interface_task.cpp`.

Code: `power_button.*` (short/long press), `battery.*` (voltage to percent), `updatePowerButton()` / `updateBattery()`
in `interface_task.cpp`. A short press opens the main menu (or returns to the app).

Not done yet: on-knob menu, long-press touch actions.
