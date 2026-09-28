"""Serial link to a LumenC3 LED controller.

Implements the binary protocol described in PROTOCOL.md. Kept free of
Home Assistant imports so it can be reused and tested on its own.
"""

from __future__ import annotations

import asyncio
from collections.abc import Callable
import contextlib
from dataclasses import dataclass
import logging

import serial_asyncio_fast

_LOGGER = logging.getLogger(__name__)

BAUDRATE = 115200
SOF = 0xAA
MAX_PAYLOAD = 16
FRAME_TIMEOUT = 0.1  # s between bytes of one frame
SIGNATURE = b"LMC3"
MIN_PROTOCOL_VERSION = 2

PING_INTERVAL = 0.5  # s between PINGs during the handshake
PING_ATTEMPTS = 12  # opening the port may reboot the board, so give it time to boot
RECONNECT_MIN = 1.0
RECONNECT_MAX = 30.0

CMD_PING = 0x00
CMD_GET_STATE = 0x01
CMD_SET_POWER = 0x02
CMD_SET_MODE = 0x03
CMD_SET_COLOR = 0x04
CMD_SET_BRIGHTNESS = 0x05
CMD_SET_SPEED = 0x06
CMD_SET_ALL = 0x07
CMD_SAVE = 0x08
RSP_PONG = 0x80
RSP_STATE = 0x81
RSP_ERROR = 0xEE

ERRORS = {
    0x01: "bad checksum",
    0x02: "unknown command",
    0x03: "bad length",
    0x04: "value out of range",
}

# Effect names shown in Home Assistant, in firmware mode order (enum Mode in
# include/config.h). Named after Yandex Smart Home scenes so the Yandex
# integration maps them automatically (it matches effect names exactly):
MODES = [
    "Solid",    # solid    — no scene: in Yandex it's just "pick a colour"
    "Fantasy",  # rainbow  → Фантазия
    "Fire",     # fire     → Свеча
    "Ocean",    # comet    → Океан
    "Garland",  # twinkle  → Гирлянда
    "Rest",     # breathe  → Отдых
    "Party",    # chase    → Вечеринка
]


class LumenC3Error(Exception):
    """Communication with the device failed."""


class NotLumenC3Error(LumenC3Error):
    """Something answered on the port, but it isn't a LumenC3."""


@dataclass(frozen=True)
class LumenC3State:
    """Device state as reported by a STATE frame."""

    on: bool
    mode: int
    rgb: tuple[int, int, int]
    brightness: int
    speed: int

    @classmethod
    def from_payload(cls, d: bytes) -> LumenC3State:
        return cls(bool(d[0]), d[1], (d[2], d[3], d[4]), d[5], d[6])


def build_frame(cmd: int, payload: bytes = b"") -> bytes:
    """SOF | CMD | LEN | PAYLOAD | XOR(CMD, LEN, PAYLOAD)."""
    cs = cmd ^ len(payload)
    for b in payload:
        cs ^= b
    return bytes([SOF, cmd, len(payload)]) + payload + bytes([cs])


_WAIT_SOF, _CMD, _LEN, _DATA, _CS = range(5)


class FrameParser:
    """Byte-by-byte frame parser; bytes outside frames (text log) are skipped."""

    def __init__(self) -> None:
        self._stage = _WAIT_SOF
        self._cmd = 0
        self._len = 0
        self._cs = 0
        self._buf = bytearray()
        self._last = 0.0

    def feed(self, data: bytes, now: float) -> list[tuple[int, bytes]]:
        frames: list[tuple[int, bytes]] = []
        for b in data:
            # drop a half-received frame if the sender went quiet
            if self._stage != _WAIT_SOF and now - self._last > FRAME_TIMEOUT:
                self._stage = _WAIT_SOF
            self._last = now

            if self._stage == _WAIT_SOF:
                if b == SOF:
                    self._stage = _CMD
            elif self._stage == _CMD:
                self._cmd = self._cs = b
                self._stage = _LEN
            elif self._stage == _LEN:
                if b > MAX_PAYLOAD:
                    self._stage = _WAIT_SOF
                    continue
                self._len = b
                self._cs ^= b
                self._buf = bytearray()
                self._stage = _DATA if b else _CS
            elif self._stage == _DATA:
                self._buf.append(b)
                self._cs ^= b
                if len(self._buf) == self._len:
                    self._stage = _CS
            else:
                self._stage = _WAIT_SOF
                if b == self._cs:
                    frames.append((self._cmd, bytes(self._buf)))
                else:
                    _LOGGER.debug("Dropped frame with bad checksum")
        return frames


class LumenC3Device:
    """One LumenC3 board on a serial port."""

    def __init__(self, port: str) -> None:
        self.port = port
        self.state: LumenC3State | None = None
        self.version: int | None = None
        self._writer: asyncio.StreamWriter | None = None
        self._reader_task: asyncio.Task[None] | None = None
        self._parser = FrameParser()
        self._pong: asyncio.Future[bytes] | None = None
        self._listeners: list[Callable[[], None]] = []
        self._was_connected = False

    @property
    def connected(self) -> bool:
        return self._writer is not None and self.version is not None

    def add_listener(self, callback: Callable[[], None]) -> Callable[[], None]:
        """Call `callback` on every state or connection change; returns a remover."""
        self._listeners.append(callback)
        return lambda: self._listeners.remove(callback)

    def _notify(self) -> None:
        for callback in list(self._listeners):
            callback()

    # ---------- connection ----------

    async def open_and_handshake(self) -> None:
        """Open the port and confirm a LumenC3 is on the other end."""
        reader, self._writer = await serial_asyncio_fast.open_serial_connection(
            url=self.port, baudrate=BAUDRATE
        )
        self._parser = FrameParser()
        self._reader_task = asyncio.create_task(self._read_loop(reader))
        loop = asyncio.get_running_loop()

        for _ in range(PING_ATTEMPTS):
            self._pong = loop.create_future()
            self._send(CMD_PING)
            done, _ = await asyncio.wait(
                {self._pong, self._reader_task},
                timeout=PING_INTERVAL,
                return_when=asyncio.FIRST_COMPLETED,
            )
            if self._reader_task in done:
                self._reader_task.result()  # re-raises the read error
                raise LumenC3Error("Serial port closed during handshake")
            if self._pong in done:
                payload = self._pong.result()
                if payload[1:5] != SIGNATURE or payload[0] < MIN_PROTOCOL_VERSION:
                    raise NotLumenC3Error(f"Unexpected PONG: {payload.hex(' ')}")
                self.version = payload[0]
                return
        raise LumenC3Error("No answer to PING")

    async def disconnect(self) -> None:
        if self._reader_task is not None:
            self._reader_task.cancel()
            with contextlib.suppress(asyncio.CancelledError, Exception):
                await self._reader_task
        if self._writer is not None:
            self._writer.close()
            with contextlib.suppress(Exception):
                await self._writer.wait_closed()
        self._reader_task = None
        self._writer = None
        self._pong = None
        self.version = None
        self._notify()

    async def run(self) -> None:
        """Keep the connection up forever, reconnecting with backoff."""
        delay = RECONNECT_MIN
        try:
            while True:
                try:
                    await self.open_and_handshake()
                    _LOGGER.info("Connected to LumenC3 on %s (protocol v%s)", self.port, self.version)
                    self._was_connected = True
                    delay = RECONNECT_MIN
                    self._send(CMD_GET_STATE)
                    self._notify()
                    assert self._reader_task is not None
                    await self._reader_task  # only ends by raising when the port dies
                except (OSError, TimeoutError, LumenC3Error) as err:
                    if self._was_connected:
                        _LOGGER.warning("Lost LumenC3 on %s: %s", self.port, err)
                        self._was_connected = False
                    else:
                        _LOGGER.debug("Can't connect to LumenC3 on %s: %s", self.port, err)
                await self.disconnect()
                await asyncio.sleep(delay)
                delay = min(delay * 2, RECONNECT_MAX)
        finally:
            await self.disconnect()

    async def _read_loop(self, reader: asyncio.StreamReader) -> None:
        loop = asyncio.get_running_loop()
        while True:
            data = await reader.read(256)
            if not data:
                raise LumenC3Error("Serial port closed")
            for cmd, payload in self._parser.feed(data, loop.time()):
                self._handle_frame(cmd, payload)

    def _handle_frame(self, cmd: int, payload: bytes) -> None:
        if cmd == RSP_PONG:
            if self._pong is not None and not self._pong.done():
                self._pong.set_result(payload)
        elif cmd == RSP_STATE and len(payload) == 7:
            self.state = LumenC3State.from_payload(payload)
            self._notify()
        elif cmd == RSP_ERROR and payload:
            _LOGGER.warning("LumenC3 rejected a command: %s", ERRORS.get(payload[0], hex(payload[0])))

    def _send(self, cmd: int, payload: bytes = b"") -> None:
        if self._writer is None:
            raise LumenC3Error("Not connected")
        self._writer.write(build_frame(cmd, payload))

    # ---------- commands ----------
    # The device answers every command with STATE, which updates self.state.

    def set_power(self, on: bool) -> None:
        self._send(CMD_SET_POWER, bytes([int(on)]))

    def set_speed(self, speed: int) -> None:
        self._send(CMD_SET_SPEED, bytes([max(1, min(100, speed))]))

    def set_all(
        self, on: bool, mode: int, rgb: tuple[int, int, int], brightness: int, speed: int
    ) -> None:
        self._send(
            CMD_SET_ALL,
            bytes([int(on), mode, *rgb, max(1, min(255, brightness)), max(1, min(100, speed))]),
        )


async def async_probe(port: str) -> int:
    """Check that a LumenC3 is on `port` and return its protocol version."""
    device = LumenC3Device(port)
    try:
        await device.open_and_handshake()
        assert device.version is not None
        return device.version
    finally:
        await device.disconnect()
