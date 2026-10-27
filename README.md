# LumenC3

Firmware for a WS2812B / SK6812 addressable LED strip on an ESP32-C3
SuperMini. Built with ESP-IDF on top of
[smart-core](https://github.com/siakinnik/smart-core), the protocol shared
by all siakinnik-home devices.

> **Work in progress.** Version 2 is a rewrite (Arduino → ESP-IDF, own
> protocol → smart-core), tested on an ESP32-C3 SuperMini over USB, Wi-Fi
> and BLE. Not done yet: signed OTA updates, the Home Assistant integration
> (siakinnik-home). Version 1 (Arduino, protocol v3, the old Home Assistant
> integration) is in the git history.

## Features

- **USB and Wi-Fi control** with the [smart-core protocol](components/smart-core/PROTOCOL.md):
  one byte-stream protocol over native USB and TCP port 7531.
- **Encrypted, paired-only network access.** Over Wi-Fi and BLE every
  connection is a Noise session (X25519, ChaCha20-Poly1305), and only
  paired hosts get in. Pair over USB, or switch the strip on 5 times in a
  row and set it up over Bluetooth, no cable needed.
- **No access point, no captive portal.** Wi-Fi credentials are `LOCAL`
  properties: set over USB, or over BLE by the host paired in pairing mode.
- **mDNS discovery** as `_smart._tcp`, hostname `lumenc3-xxxx.local`.
- **7 effects** — solid, rainbow, fire, comet, twinkle, breathe, chase.
- **PSU current limit** — brightness is scaled to stay under `LED_MAX_MILLIAMPS`.
- **Settings survive reboots** (NVS), saved 3 s after the last change.
- **Two OTA slots** in the partition table, ready for over-the-air updates.

## Properties

Model id `0x0001` (`lumenc3`). Ids are fixed; new ones are only appended.

| Id | Name               | Type    | Flags             | Range / values |
|----|--------------------|---------|-------------------|----------------|
| 0  | `light.power`      | bool    | rw, persist       |                |
| 1  | `light.brightness` | u8      | rw, persist       | 1–255          |
| 2  | `light.color`      | rgb     | rw, persist       |                |
| 3  | `light.effect`     | enum    | rw, persist       | solid, rainbow, fire, comet, twinkle, breathe, chase |
| 4  | `light.speed`      | u8      | rw, persist       | 1–100          |
| 5  | `sys.reboot`       | action  |                   |                |
| 6  | `sys.reset`        | action  | local             | factory reset  |
| 7  | `sys.wifi_ssid`    | str 32  | rw, local, persist|                |
| 8  | `sys.wifi_pass`    | str 64  | w, secret, local, persist |        |
| 9  | `sys.wifi`         | enum    | r                 | unconfigured, connecting, connected |
| 10 | `sys.ip`           | str 15  | r                 |                |
| 11 | `sys.wifi_error`   | enum    | r                 | none, wrong_password, no_network, failed: why the last attempt failed |

## Hardware

- ESP32-C3 SuperMini (4 MB flash)
- WS2812B (or compatible) strip on GPIO4
- 5 V PSU sized for the strip; common ground with the board

Pins, strip length, LED type and the current limit are in
[`main/config.h`](main/config.h).

## Build

Built and tested with ESP-IDF **v6.1** (5.3 or newer should work).

```bash
git clone --recurse-submodules https://github.com/siakinnik/LumenC3
cd LumenC3
idf.py set-target esp32c3
idf.py build flash monitor
```

`led_strip` and `mdns` are fetched by the IDF component manager on the
first build.

## Connect to Wi-Fi

Plug the board in over USB and set the credentials with `smartctl.py`,
the smart-core command-line client in `components/smart-core/clients/python`
(needs Python 3.10+ and `pip install pyserial`):

```bash
python smartctl.py serial:COM5 set sys.wifi_ssid=MyNetwork sys.wifi_pass=secret
python smartctl.py serial:COM5 get sys.wifi sys.ip
```

On Linux the port is `/dev/ttyACM0`. Close `idf.py monitor` first: only one
program can hold the port.

## Pairing

Over USB the board trusts whoever holds the cable. Over Wi-Fi it only talks
to hosts it has been paired with; everything else is refused before a
single command is read.

**Over USB** (simplest):

```bash
python smartctl.py serial:COM5 pair
```

**Without USB**, the power switch is the button:

| Switch on … times in a row (each time off again within 5 s) | Result |
|---|---|
| 5 | **Pairing mode**: forgets the Wi-Fi network and every paired host, turns on Bluetooth and opens the pairing window for 3 minutes. The strip breathes blue |
| 10 | Factory reset: Wi-Fi, paired hosts, the device key and all settings are erased |

In pairing mode, from a computer with Bluetooth:

```bash
python smartctl.py ble scan                                # finds LumenC3-62B2
python smartctl.py ble:LumenC3-62B2 setup --ssid MyNetwork  # asks for the Wi-Fi password
```

`setup` pairs the computer over Bluetooth, gives the strip the network and
waits until it's online. Only the host paired in this window may set the
Wi-Fi, and only until the window ends. Bluetooth turns off with the window.

Two green flashes: paired. Three red flashes: a second, unknown host tried
to pair in the same window, so the board trusted neither; start pairing
mode again. Only real power-ons count: a reboot or a crash never does.

```bash
python smartctl.py tcp:lumenc3-62b2.local unpair   # forget this computer
python smartctl.py serial:COM5 unpair --all        # USB: forget every host
```

`smartctl` keeps this computer's key and the boards it paired with in
`%APPDATA%\smart-core\identity.json` (`~/.config/smart-core/identity.json`
on Linux). Keep that file private.

## Control

```bash
python smartctl.py serial:COM5 info                      # every property, the device key
python smartctl.py serial:COM5 set light.effect=fire light.brightness=0x80
python smartctl.py serial:COM5 set light.color=#00ff40 light.effect=solid
python smartctl.py tcp:lumenc3-62b2.local watch          # print changes as they happen
python smartctl.py serial:COM5 trigger sys.reboot
```
