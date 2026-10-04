"""Config flow for LumenC3: discovery over USB and Wi-Fi (zeroconf), plus manual setup.

USB ports are never opened on discovery alone: every ESP32-C3 with native USB
has the same VID/PID (303A:1001), so a discovered port may belong to something
else (e.g. a Zigbee/Thread stick). The port is only opened for a PING after
the user confirms, and ports referenced by other integrations are skipped.

A board is identified by its MAC: it's the USB serial number, the "id" in its
mDNS record and part of its PONG, so the same board found over USB and over
Wi-Fi is recognised as one device.
"""

from __future__ import annotations

from collections.abc import Iterable
from typing import Any

from serial.tools import list_ports
from serial.tools.list_ports_common import ListPortInfo
import voluptuous as vol

from homeassistant.components import usb
from homeassistant.config_entries import ConfigFlow, ConfigFlowResult
from homeassistant.core import HomeAssistant
from homeassistant.helpers.service_info.usb import UsbServiceInfo
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo

from .const import CONF_HOST, CONF_PORT, CONF_TCP_PORT, DOMAIN, ESP_USB_PID, ESP_USB_VID
from .device import DEFAULT_TCP_PORT, LumenC3Device, LumenC3Error, NotLumenC3Error, async_probe


def _strings(value: Any) -> Iterable[str]:
    """All string values in a (nested) config entry data dict."""
    if isinstance(value, str):
        yield value
    elif isinstance(value, dict):
        for v in value.values():
            yield from _strings(v)
    elif isinstance(value, (list, tuple)):
        for v in value:
            yield from _strings(v)


def _port_in_use(hass: HomeAssistant, paths: set[str]) -> bool:
    """True if another integration's config entry already references one of `paths`."""
    for entry in hass.config_entries.async_entries():
        if entry.domain == DOMAIN:
            continue
        if paths & set(_strings(entry.data)) or paths & set(_strings(entry.options)):
            return True
    return False


def _is_esp_native_usb(port: ListPortInfo) -> bool:
    return (port.vid, port.pid) == (ESP_USB_VID, ESP_USB_PID)


def _port_label(port: ListPortInfo) -> str:
    """E.g. "/dev/ttyACM0 — ESP32 (possibly LumenC3), S/N A0:76:4E:12:34:56"."""
    kind = "ESP32 (possibly LumenC3)" if _is_esp_native_usb(port) else (
        port.product or port.description or f"USB {port.vid:04X}:{port.pid:04X}"
    )
    label = f"{port.device} — {kind}"
    if port.serial_number:
        label += f", S/N {port.serial_number}"
    return label


class LumenC3ConfigFlow(ConfigFlow, domain=DOMAIN):
    VERSION = 1

    def __init__(self) -> None:
        # {CONF_PORT: path} for USB, {CONF_HOST: ..., CONF_TCP_PORT: ...} for Wi-Fi
        self._data: dict[str, Any] = {}

    @property
    def _address(self) -> str:
        return self._data.get(CONF_PORT) or f"{self._data[CONF_HOST]}:{self._data[CONF_TCP_PORT]}"

    def _device(self) -> LumenC3Device:
        if CONF_HOST in self._data:
            return LumenC3Device(host=self._data[CONF_HOST], tcp_port=self._data[CONF_TCP_PORT])
        return LumenC3Device(serial_port=self._data[CONF_PORT])

    async def _async_probe(self) -> str | None:
        """Connect and PING; on success the unique ID is the board's MAC. Returns an error key or None."""
        device = self._device()
        try:
            await async_probe(device)
        except NotLumenC3Error:
            return "not_lumenc3"
        except (OSError, TimeoutError, LumenC3Error):
            return "cannot_connect"
        await self.async_set_unique_id(
            device.device_id or self.unique_id or self._address, raise_on_progress=False
        )
        return None

    def _abort_if_configured(self) -> None:
        """Abort if this board is already set up.

        If it's set up over the same transport, follow it to its new address
        (another USB port, a new IP). An existing USB entry is never switched
        to Wi-Fi or back by discovery.
        """
        if self.unique_id is None:
            return
        entry = self.hass.config_entries.async_entry_for_domain_unique_id(DOMAIN, self.unique_id)
        same_transport = entry is not None and (CONF_HOST in entry.data) == (CONF_HOST in self._data)
        self._abort_if_unique_id_configured(updates=self._data if same_transport else None)

    def _create_entry(self) -> ConfigFlowResult:
        return self.async_create_entry(title="LumenC3", data=self._data)

    # ---------- discovery ----------

    async def async_step_usb(self, discovery_info: UsbServiceInfo) -> ConfigFlowResult:
        dev_path = discovery_info.device
        port = await self.hass.async_add_executor_job(usb.get_serial_by_id, dev_path)
        if _port_in_use(self.hass, {dev_path, port}):
            return self.async_abort(reason="port_in_use")

        self._data = {CONF_PORT: port}
        if discovery_info.serial_number:
            await self.async_set_unique_id(discovery_info.serial_number.upper())
            self._abort_if_configured()
        self.context["title_placeholders"] = {"name": f"LumenC3 ({dev_path})"}
        return await self.async_step_confirm()

    async def async_step_zeroconf(self, discovery_info: ZeroconfServiceInfo) -> ConfigFlowResult:
        device_id = discovery_info.properties.get("id")
        if not device_id:
            return self.async_abort(reason="not_lumenc3")

        self._data = {
            CONF_HOST: discovery_info.host,
            CONF_TCP_PORT: discovery_info.port or DEFAULT_TCP_PORT,
        }
        await self.async_set_unique_id(device_id.upper())
        self._abort_if_configured()
        self.context["title_placeholders"] = {"name": f"LumenC3 ({discovery_info.host})"}
        return await self.async_step_confirm()

    async def async_step_confirm(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        errors: dict[str, str] = {}
        if user_input is not None:
            error = await self._async_probe()
            if error == "not_lumenc3":
                return self.async_abort(reason=error)
            if error is None:
                self._abort_if_configured()
                return self._create_entry()
            errors["base"] = error

        self._set_confirm_only()
        return self.async_show_form(
            step_id="confirm",
            description_placeholders={"address": self._address},
            errors=errors,
        )

    # ---------- manual setup ----------

    async def async_step_user(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        return self.async_show_menu(step_id="user", menu_options=["serial", "network"])

    async def async_step_serial(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        errors: dict[str, str] = {}
        # only USB ports: skips the dozens of legacy /dev/ttyS* Linux always creates
        ports = [
            p for p in await self.hass.async_add_executor_job(list_ports.comports)
            if p.vid is not None
        ]
        # ESP32s with native USB (a LumenC3 is one of them) first
        ports.sort(key=lambda p: (not _is_esp_native_usb(p), p.device))
        configured = {
            entry.data.get(CONF_PORT) for entry in self._async_current_entries(include_ignore=False)
        }
        by_id = {
            p.device: await self.hass.async_add_executor_job(usb.get_serial_by_id, p.device)
            for p in ports
        }
        available = {
            p.device: _port_label(p)
            for p in ports
            if by_id[p.device] not in configured
            and not _port_in_use(self.hass, {p.device, by_id[p.device]})
        }
        if not available:
            return self.async_abort(reason="no_ports")

        if user_input is not None:
            dev_path = user_input[CONF_PORT]
            info = next(p for p in ports if p.device == dev_path)
            self._data = {CONF_PORT: by_id[dev_path]}
            if info.serial_number:
                await self.async_set_unique_id(info.serial_number.upper())
                self._abort_if_configured()

            error = await self._async_probe()
            if error is None:
                self._abort_if_configured()
                return self._create_entry()
            errors["base"] = error

        return self.async_show_form(
            step_id="serial",
            data_schema=vol.Schema({vol.Required(CONF_PORT): vol.In(available)}),
            errors=errors,
        )

    async def async_step_network(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        errors: dict[str, str] = {}
        if user_input is not None:
            self._data = {
                CONF_HOST: user_input[CONF_HOST].strip(),
                CONF_TCP_PORT: user_input[CONF_TCP_PORT],
            }
            error = await self._async_probe()
            if error is None:
                self._abort_if_configured()
                return self._create_entry()
            errors["base"] = error

        return self.async_show_form(
            step_id="network",
            data_schema=vol.Schema(
                {
                    vol.Required(CONF_HOST, default=self._data.get(CONF_HOST, "")): str,
                    vol.Required(CONF_TCP_PORT, default=DEFAULT_TCP_PORT): vol.All(
                        vol.Coerce(int), vol.Range(min=1, max=65535)
                    ),
                }
            ),
            errors=errors,
        )
