"""Base entity for LumenC3."""

from __future__ import annotations

from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.entity import Entity

from . import LumenC3ConfigEntry
from .const import DOMAIN
from .device import LumenC3Device


class LumenC3Entity(Entity):
    """Entity backed by a LumenC3Device; updated by frames the board pushes."""

    _attr_has_entity_name = True
    _attr_should_poll = False

    def __init__(self, entry: LumenC3ConfigEntry, key: str) -> None:
        self._device: LumenC3Device = entry.runtime_data
        self._attr_unique_id = f"{entry.unique_id}_{key}"
        self._attr_device_info = DeviceInfo(
            identifiers={(DOMAIN, str(entry.unique_id))},
            name="LumenC3",
            manufacturer="LumenC3",
            model="ESP32-C3 LED strip controller",
            sw_version=f"protocol v{self._device.version}",
        )

    @property
    def available(self) -> bool:
        return self._device.connected and self._device.state is not None

    async def async_added_to_hass(self) -> None:
        self.async_on_remove(self._device.add_listener(self.async_write_ha_state))
