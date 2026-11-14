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
- `firmware/src/touch_ring.*`: click-wheel style sliding around the rim; tuning at the top of `touch_ring.h`
- `firmware/src/interface_task.*`: what each gesture does (`updateTouch()`), when `SK_TOUCH=1`

## Touch gestures

Every gesture is printed in the monitor, e.g. `Touch: DOUBLE_TAP at (120, 98)`, so the monitor doubles as a touch test mode.

| Gesture | Feel | Does |
|---|---|---|
| Tap | firm click | next mode |
| Double tap | two firm clicks | previous mode |
| Triple tap | three firm clicks | nothing yet (reserved) |
| Long press (~0.6 s) | firm click while still held | nothing yet (reserved) |
| Swipe left / right | light click | next / previous mode |
| Swipe up / down | none | nothing yet (reserved) |
| Slide around the rim | light click per step | test only: prints `Ring: +1 (angle 93)` |

Single and double taps wait ~0.25 s to make sure another tap isn't coming.

The rim slider (`touch_ring.*`) counts a step every 15 degrees while your finger is in the outer band of the screen
(more than 75 px from center). Clockwise is +. A touch used as a rim slide isn't also reported as a swipe or tap.

Not done yet: the Function button, Bluetooth control of the Mac/iPad.
