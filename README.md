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
- **Home Assistant integration** (HACS) over USB serial or Wi-Fi, with
  auto-discovery for both — see [below](#home-assistant).
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

The integration talks to the board with the [control protocol](PROTOCOL.md),
either way:

- **USB** — plug the board into the machine running Home Assistant. No
  Wi-Fi needed. Firmware with protocol **v2** or newer.
- **Wi-Fi** — the board just has to be on the same network. Firmware with
  protocol **v3** or newer.

### Install with HACS

1. HACS → ⋮ → **Custom repositories** → add
   `https://github.com/siakinnik/LumenC3`, type **Integration**.
2. Install **LumenC3** and restart Home Assistant.

Manual install: copy `custom_components/lumenc3` into your
`config/custom_components/` folder and restart.

### Setup

- **Auto-discovery** — when the board is plugged in over USB or appears on
  the network (mDNS `_lumenc3._tcp`), Home Assistant shows
  *Discovered: LumenC3*. Press **Add** to confirm.
- **Manually** — Settings → Devices & services → Add integration → LumenC3,
  then choose **USB** and pick the port, or **Wi-Fi** and enter the IP
  (printed in the board's log as `Wi-Fi OK: …`).

The board is identified by its MAC, so if it's found both over USB and over
Wi-Fi it's still one device: once it's set up one way, discovery the other
way is skipped. If its IP changes, the Wi-Fi entry follows it automatically.

Over Wi-Fi the integration pings the board every 30 s, so a board that lost
power shows up as unavailable instead of silently stale.

### USB discovery and other ESP32 devices

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

Changes made elsewhere (web UI, another USB or Wi-Fi host) show up in Home
Assistant right away: the board pushes its state to every connected host.

Effects are named after Yandex scenes, so the
[Yandex Smart Home](https://github.com/dext0r/yandex_smart_home) integration
maps them automatically, no `entity_config` needed:

| Firmware mode | HA effect | Yandex scene |
|---------------|-----------|--------------|
| solid         | Solid     | — (pick a colour; setting a colour in Fantasy/Fire switches to Solid) |
| rainbow       | Fantasy   | Фантазия     |
| fire          | Fire      | Свеча        |
| comet         | Ocean     | Океан        |
| twinkle       | Garland   | Гирлянда     |
| breathe       | Rest      | Отдых        |
| chase         | Party     | Вечеринка    |

Over USB, only one program can hold the port at a time: close the serial
monitor and `tools/serial-control.html` while Home Assistant is connected.
Over Wi-Fi, up to 3 hosts can be connected at once.

### Docker

For USB, mount `/dev` and `/run/udev` so devices survive replugging and USB
discovery works. Passing a single `--device /dev/ttyACM0` only gives the
container a snapshot taken at startup:

```yaml
    privileged: true            # or device_cgroup_rules: ['c 166:* rmw']
    volumes:
      - /dev:/dev
      - /run/udev:/run/udev:ro
```

For Wi-Fi discovery the container needs to see mDNS, e.g. with
`network_mode: host`.
