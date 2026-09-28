"""Config flow for LumenC3: USB discovery plus manual port selection.

Ports are never opened on discovery alone: every ESP32-C3 with native USB has
the same VID/PID (303A:1001), so a discovered port may belong to something
else (e.g. a Zigbee/Thread stick). The port is only opened for a PING after
the user confirms, and ports referenced by other integrations are skipped.
"""

from __future__ import annotations

from collections.abc import Iterable
from typing import Any

from serial.tools import list_ports
import voluptuous as vol

from homeassistant.components import usb
from homeassistant.config_entries import ConfigFlow, ConfigFlowResult
from homeassistant.core import HomeAssistant
from homeassistant.helpers.service_info.usb import UsbServiceInfo

from .const import CONF_PORT, DOMAIN
from .device import LumenC3Error, NotLumenC3Error, async_probe


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


class LumenC3ConfigFlow(ConfigFlow, domain=DOMAIN):
    VERSION = 1

    def __init__(self) -> None:
        self._port: str | None = None

    async def _async_probe(self, port: str) -> str | None:
        """Open the port and PING it; returns an error key or None."""
        try:
            await async_probe(port)
        except NotLumenC3Error:
            return "not_lumenc3"
        except (OSError, TimeoutError, LumenC3Error):
            return "cannot_connect"
        return None

    def _create_entry(self) -> ConfigFlowResult:
        assert self._port is not None
        return self.async_create_entry(title="LumenC3", data={CONF_PORT: self._port})

    # ---------- USB discovery ----------

    async def async_step_usb(self, discovery_info: UsbServiceInfo) -> ConfigFlowResult:
        dev_path = discovery_info.device
        port = await self.hass.async_add_executor_job(usb.get_serial_by_id, dev_path)
        if _port_in_use(self.hass, {dev_path, port}):
            return self.async_abort(reason="port_in_use")

        await self.async_set_unique_id(discovery_info.serial_number or port)
        # same board plugged into another port: just follow it
        self._abort_if_unique_id_configured(updates={CONF_PORT: port})

        self._port = port
        self.context["title_placeholders"] = {"name": f"LumenC3 ({dev_path})"}
        return await self.async_step_confirm()

    async def async_step_confirm(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        assert self._port is not None
        errors: dict[str, str] = {}
        if user_input is not None:
            error = await self._async_probe(self._port)
            if error == "not_lumenc3":
                return self.async_abort(reason=error)
            if error is None:
                return self._create_entry()
            errors["base"] = error

        self._set_confirm_only()
        return self.async_show_form(
            step_id="confirm",
            description_placeholders={"port": self._port},
            errors=errors,
        )

    # ---------- manual setup ----------

    async def async_step_user(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        errors: dict[str, str] = {}
        ports = await self.hass.async_add_executor_job(list_ports.comports)
        configured = {
            entry.data.get(CONF_PORT) for entry in self._async_current_entries(include_ignore=False)
        }
        by_id = {
            p.device: await self.hass.async_add_executor_job(usb.get_serial_by_id, p.device)
            for p in ports
        }
        available = {
            p.device: f"{p.device} — {p.description}" if p.description else p.device
            for p in ports
            if by_id[p.device] not in configured
            and not _port_in_use(self.hass, {p.device, by_id[p.device]})
        }
        if not available:
            return self.async_abort(reason="no_ports")

        if user_input is not None:
            dev_path = user_input[CONF_PORT]
            info = next(p for p in ports if p.device == dev_path)
            self._port = by_id[dev_path]
            await self.async_set_unique_id(info.serial_number or self._port)
            self._abort_if_unique_id_configured(updates={CONF_PORT: self._port})

            error = await self._async_probe(self._port)
            if error is None:
                return self._create_entry()
            errors["base"] = error

        return self.async_show_form(
            step_id="user",
            data_schema=vol.Schema({vol.Required(CONF_PORT): vol.In(available)}),
            errors=errors,
        )
