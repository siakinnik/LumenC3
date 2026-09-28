# LumenC3 - WIP(schematics, documentation and video)

Firmware for driving a WS2812B / SK6812 addressable LED strip from an ESP32-C3 SuperMini. Built with PlatformIO and FastLED.

> **Wi-Fi not tested.** The captive portal, web UI and mDNS are implemented
> but haven't been verified on real hardware yet (my board has a
> broken radio). Serial control is the tested way to drive the strip.

## Features

- **Serial control** — binary protocol over USB for power, mode, colour,
  brightness and speed; works without Wi-Fi. See [PROTOCOL.md](PROTOCOL.md).
  A ready-made control panel is in [tools/serial-control.html](tools/serial-control.html) —
  open it in Chrome or Edge, click **Connect** and pick the board's port.
- **Captive-portal Wi-Fi setup** *(not tested)* — on first boot the board
  hosts a `LED-Strip-Setup` access point; connect, pick your network, done.
  No credentials hard-coded in the source. The portal is non-blocking: the
  strip and Serial control keep working while it's up.
- **Web UI** at `http://ledstrip.local` *(not tested)* — power, brightness,
  speed, colour picker and mode selection from any phone.
- **7 animation modes** — solid, rainbow, Fire2012, comet, twinkle,
  breathe, theater chase.
- **Serial reset** — send a configurable magic byte sequence over USB
  to wipe the saved Wi-Fi and settings.
- **All hardware settings in one place** (`include/config.h`): data pin,
  strip length, LED type, colour order, PSU current limit.
- Persists the last mode/brightness/colour to flash.

## Hardware

- ESP32-C3 SuperMini
- WS2812B (or compatible) addressable strip on GPIO4
- 5 V PSU sized for the strip; common ground with the board

## Build

```bash
pio run -t upload
pio device monitor
```
