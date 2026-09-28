"""LumenC3 animation speed."""

from __future__ import annotations

from homeassistant.components.number import NumberEntity, NumberMode
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from . import LumenC3ConfigEntry
from .device import LumenC3Error
from .entity import LumenC3Entity


async def async_setup_entry(
    hass: HomeAssistant, entry: LumenC3ConfigEntry, async_add_entities: AddEntitiesCallback
) -> None:
    async_add_entities([LumenC3Speed(entry, "speed")])


class LumenC3Speed(LumenC3Entity, NumberEntity):
    _attr_translation_key = "speed"
    _attr_icon = "mdi:speedometer"
    _attr_native_min_value = 1
    _attr_native_max_value = 100
    _attr_native_step = 1
    _attr_mode = NumberMode.SLIDER

    @property
    def native_value(self) -> float | None:
        s = self._device.state
        return s.speed if s else None

    async def async_set_native_value(self, value: float) -> None:
        try:
            self._device.set_speed(round(value))
        except LumenC3Error as err:
            raise HomeAssistantError(str(err)) from err
