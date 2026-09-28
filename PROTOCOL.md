# LumenC3 Serial Protocol

Binary protocol for controlling the strip over the USB Serial port (USB-CDC).
Works without Wi-Fi.

- Port settings: **115200 baud, 8N1** (baud rate is ignored by USB-CDC, but set it anyway)
- Protocol version: **1** (returned by `PING`)
- All values are single bytes; multi-byte fields are sent in the order listed.

## Frame format

Requests and responses use the same frame:

| Offset | Size  | Field     | Description                                      |
|--------|-------|-----------|--------------------------------------------------|
| 0      | 1     | `SOF`     | Start of frame, always `0xAA`                    |
| 1      | 1     | `CMD`     | Command / response code                          |
| 2      | 1     | `LEN`     | Payload length, `0..16`                          |
| 3      | `LEN` | `PAYLOAD` | Command data                                     |
| 3+LEN  | 1     | `CS`      | Checksum: `CMD ^ LEN ^ PAYLOAD[0] ^ … ^ PAYLOAD[LEN-1]` |

`SOF` is not part of the checksum.

A frame must be sent in one go: if more than **100 ms** pass between two
bytes of the same frame, the partial frame is dropped and the parser waits
for the next `0xAA`.

### Text output on the same port

The firmware also prints human-readable ASCII log lines to the same port
(`LED strip: starting`, `Wi-Fi OK: …`, WiFiManager debug output, etc.).
All of it is below `0x80`, so `0xAA` never appears there. A host should
skip every byte until it sees `0xAA`, then read a full frame and verify
the checksum.

## Commands (host → device)

| CMD    | Name             | LEN | Payload                                              | Response |
|--------|------------------|-----|------------------------------------------------------|----------|
| `0x00` | `PING`           | 0   | —                                                    | `PONG`   |
| `0x01` | `GET_STATE`      | 0   | —                                                    | `STATE`  |
| `0x02` | `SET_POWER`      | 1   | `on` — `0` = off, anything else = on                 | `STATE`  |
| `0x03` | `SET_MODE`       | 1   | `mode` — see [Modes](#modes)                         | `STATE`  |
| `0x04` | `SET_COLOR`      | 3   | `r`, `g`, `b` — `0..255` each                        | `STATE`  |
| `0x05` | `SET_BRIGHTNESS` | 1   | `brightness` — `1..255`                              | `STATE`  |
| `0x06` | `SET_SPEED`      | 1   | `speed` — `1..100`                                   | `STATE`  |
| `0x07` | `SET_ALL`        | 7   | `on`, `mode`, `r`, `g`, `b`, `brightness`, `speed`   | `STATE`  |
| `0x08` | `SAVE`           | 0   | —                                                    | `STATE`  |

- Changes are applied immediately and written to flash automatically
  **3 s after the last change**. `SAVE` writes them right away.
- `SET_ALL` validates every field first; if any value is out of range,
  nothing is changed.
- `SET_POWER` with `on = 0` blacks the strip out but keeps the rest of the state.

## Responses (device → host)

| CMD    | Name    | LEN | Payload                                            |
|--------|---------|-----|----------------------------------------------------|
| `0x80` | `PONG`  | 1   | `version` — protocol version (`1`)                 |
| `0x81` | `STATE` | 7   | `on`, `mode`, `r`, `g`, `b`, `brightness`, `speed` |
| `0xEE` | `ERROR` | 1   | `code` — see [Error codes](#error-codes)           |

Every valid request gets exactly one response. The `STATE` payload has the
same layout as the `SET_ALL` payload, so a state that has been read can be
sent back unchanged.

### Error codes

| Code   | Name          | Meaning                                              |
|--------|---------------|------------------------------------------------------|
| `0x01` | `CHECKSUM`    | Checksum mismatch                                    |
| `0x02` | `UNKNOWN_CMD` | Unknown `CMD`                                        |
| `0x03` | `LENGTH`      | `LEN` is wrong for this command, or greater than 16  |
| `0x04` | `VALUE`       | A value is out of range (mode, brightness, speed)    |

A request that got an `ERROR` response has changed nothing.

## Modes

| Value | Mode      | Uses `color` |
|-------|-----------|--------------|
| `0`   | solid     | yes          |
| `1`   | rainbow   | no           |
| `2`   | fire      | no           |
| `3`   | comet     | yes          |
| `4`   | twinkle   | yes          |
| `5`   | breathe   | yes          |
| `6`   | chase     | yes          |

## Examples

All bytes in hex.

| Action                                  | Request                                  |
|-----------------------------------------|------------------------------------------|
| Ping                                    | `AA 00 00 00`                            |
| Read state                              | `AA 01 00 01`                            |
| Power on                                | `AA 02 01 01 02`                         |
| Power off                               | `AA 02 01 00 03`                         |
| Mode = rainbow                          | `AA 03 01 01 03`                         |
| Mode = fire                             | `AA 03 01 02 00`                         |
| Color = red                             | `AA 04 03 FF 00 00 F8`                   |
| Brightness = 128                        | `AA 05 01 80 84`                         |
| Speed = 50                              | `AA 06 01 32 35`                         |
| On, fire, color FF3C00, bright 160, speed 50 | `AA 07 07 01 02 FF 3C 00 A0 32 52`  |
| Save now                                | `AA 08 00 08`                            |

Responses:

| Response                     | Bytes             |
|------------------------------|-------------------|
| `PONG`, version 1            | `AA 80 01 01 80`  |
| `ERROR`, bad checksum        | `AA EE 01 01 EE`  |

### Checksum walkthrough

`SET_COLOR` red — `CMD=04`, `LEN=03`, payload `FF 00 00`:

```
04 ^ 03 = 07
07 ^ FF = F8
F8 ^ 00 = F8
F8 ^ 00 = F8   → CS = F8
```

## Browser control panel

[tools/serial-control.html](tools/serial-control.html) is a single-file
page that talks this protocol through the Web Serial API — no server or
install needed. Open it directly from disk in Chrome or Edge (desktop),
click **Connect** and choose the board's port. It also shows the device's
text log and every frame sent and received.

## Python example

Requires `pyserial` (`pip install pyserial`). Set the port to the board's COM port.

```python
import serial

SOF = 0xAA

def frame(cmd, payload=b""):
    cs = cmd ^ len(payload)
    for b in payload:
        cs ^= b
    return bytes([SOF, cmd, len(payload)]) + bytes(payload) + bytes([cs])

def read_frame(port):
    # skip log text until the start of a frame
    while True:
        b = port.read(1)
        if not b:
            raise TimeoutError("no response")
        if b[0] == SOF:
            break
    cmd, length = port.read(2)
    payload = port.read(length)
    cs = port.read(1)[0]
    calc = cmd ^ length
    for b in payload:
        calc ^= b
    if calc != cs:
        raise ValueError("bad checksum")
    return cmd, payload

with serial.Serial("COM5", 115200, timeout=1) as port:
    port.write(frame(0x03, [2]))              # mode = fire
    cmd, data = read_frame(port)
    if cmd == 0x81:
        on, mode, r, g, b, bright, speed = data
        print(f"on={on} mode={mode} color=#{r:02x}{g:02x}{b:02x} "
              f"brightness={bright} speed={speed}")
    elif cmd == 0xEE:
        print("error", hex(data[0]))
```

## Factory reset

Separately from the protocol, the firmware watches the raw byte stream for
the reset sequence defined by `RESET_MAGIC` in `include/config.h`
(default `5A A5 C3 3C DE AD`). Receiving it erases saved Wi-Fi credentials
and settings and reboots the board. It is not framed and gets no response.
