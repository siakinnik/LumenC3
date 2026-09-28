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
- **Home Assistant integration** (HACS) over USB serial, with auto-discovery —
  see [below](#home-assistant).
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

## Home Assistant

Plug the board into the machine running Home Assistant over USB. The
integration talks to it with the [serial protocol](PROTOCOL.md) and needs
firmware with protocol **v2** or newer. No Wi-Fi is needed.

### Install with HACS

1. HACS → ⋮ → **Custom repositories** → add
   `https://github.com/siakinnik/LumenC3`, type **Integration**.
2. Install **LumenC3** and restart Home Assistant.

Manual install: copy `custom_components/lumenc3` into your
`config/custom_components/` folder and restart.

### Setup

- **Auto-discovery** — when the board is plugged in, Home Assistant shows
  *Discovered: LumenC3*. Press **Add** to confirm.
- **Manually** — Settings → Devices & services → Add integration → LumenC3,
  then pick the port.

Every ESP32-C3 with native USB reports the same USB ID (`303A:1001`), so
discovery can't tell a LumenC3 from other ESP32 devices by USB alone. To
avoid disturbing them, the integration:

- never opens a port on discovery alone; it sends a `PING` only after you
  confirm, and checks the `LMC3` signature in the reply;
- skips ports already referenced by another integration (for example ZHA
  or a Zigbee/Thread stick).

If a discovered device isn't a LumenC3, press **Ignore**.

Opening the port may reboot the board; the integration waits for it to
come back. Settings survive the reboot.

### Entities

| Entity           | What it controls                                           |
|------------------|------------------------------------------------------------|
| `light.lumenc3`  | Power, brightness, RGB colour; animation modes as effects  |
| `number.lumenc3_speed` | Animation speed, 1–100                               |

Changes made from the web UI show up in Home Assistant right away: the
board pushes its state over serial.

Only one program can hold the port at a time: close the serial monitor
and `tools/serial-control.html` while Home Assistant is connected.

If Home Assistant runs in Docker, pass the device through, for example
`--device /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_…`.
