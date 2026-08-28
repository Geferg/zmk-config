# Space OLED HUD — first-pass flash

This is the first custom pass for the Corne setup discussed in chat:

- `nice_nano_v2`
- `corne_left nice_oled`
- `corne_right nice_oled`
- 128×32 SSD1306 mounted vertically
- ZMK `v0.3.0`
- `mctechnology17/zmk-nice-oled`

The `nice_oled` shield is **kept** for OLED/I²C hardware setup, but its stock
presentation is disabled. This module supplies `zmk_display_status_screen()`.

## What this pass does

### Left / central — Mission Control

- `TYPING` for layers 0–3
- `GAMING` for layers 4–7
- Saturn-style planet
- tiny orbiting moon
- current sub-layer (`BASE`, `LOWER`, `RAISE`, `ADJUST`, `ALT`)
- `USB` or Bluetooth profile (`B1`, `B2`, ...)
- local battery
- four-position layer strip
- segmented local battery bar

### Right / peripheral — Flight View

- `VELOCITY`
- large outlined spacecraft
- animated exhaust
- moving star field
- thrust meter
- local battery
- segmented local battery bar

**First-pass limitation:** `VELOCITY` is derived from keypress activity generated
on the **right half**, not the central's true global WPM. ZMK v0.3 does not
already relay the central WPM state to a normal peripheral. This lets us test the
visual/animation immediately; true WPM relay is a good Stage 2 change.

## 1. Add the module files to your zmk-config repository

Copy:

```text
Kconfig
CMakeLists.txt
src/space_status.c
```

to the root of your ZMK config repo.

### `zephyr/module.yml`

If you **do not already have** `zephyr/module.yml`, copy the supplied one.

If you already have one, **merge** these keys into its existing `build:` block
rather than overwriting the file:

```yaml
name: space-oled-hud

build:
  cmake: .
  kconfig: Kconfig
```

Keep any existing settings such as `board_root`, `dts_root`, or `snippet_root`.

For example, an existing unified-config module could end up as:

```yaml
name: space-oled-hud

build:
  cmake: .
  kconfig: Kconfig
  settings:
    board_root: .
    dts_root: .
```

Likewise, if your repo already has a root `Kconfig` or `CMakeLists.txt`, append
the supplied entries instead of deleting existing content.

## 2. Update your `.conf`

Use the supplied `config-snippet.conf` as the display/RGB section.

The two critical lines that hand presentation over to this module are:

```conf
CONFIG_SPACE_OLED_HUD=y
CONFIG_NICE_OLED_WIDGET_STATUS=n
```

Keep:

```conf
CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM=y
```

## 3. Do NOT change build.yaml

Keep:

```yaml
---
include:
  - board: nice_nano_v2
    shield: corne_left nice_oled
  - board: nice_nano_v2
    shield: corne_right nice_oled
```

The `nice_oled` shield is still doing useful hardware setup.

## 4. Do NOT remove zmk-nice-oled from west.yml

We still use the module's `nice_oled` shield and its OLED configuration.

## Expected first result

The exact pixel art will need tuning on the physical OLED, but this should give
us a real first hardware pass rather than another mock-up. The most useful
feedback after flashing is:

1. Is orientation correct on both halves?
2. Is the text readable at physical size?
3. Does the planet occupy the right amount of screen?
4. Is the ship too large/small?
5. Does the animation feel smooth?
6. Does TYPING/GAMING switch immediately?
7. How responsive does VELOCITY feel while typing?
