"""LumenC3 light: power, brightness, colour and animation mode."""

from __future__ import annotations

from typing import Any

from homeassistant.components.light import (
    ATTR_BRIGHTNESS,
    ATTR_EFFECT,
    ATTR_RGB_COLOR,
    ColorMode,
    LightEntity,
    LightEntityFeature,
)
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from . import LumenC3ConfigEntry
from .device import MODES, LumenC3Error
from .entity import LumenC3Entity


async def async_setup_entry(
    hass: HomeAssistant, entry: LumenC3ConfigEntry, async_add_entities: AddEntitiesCallback
) -> None:
    async_add_entities([LumenC3Light(entry, "light")])


class LumenC3Light(LumenC3Entity, LightEntity):
    _attr_name = None  # the device's main feature: named after the device
    _attr_color_mode = ColorMode.RGB
    _attr_supported_color_modes = {ColorMode.RGB}
    _attr_supported_features = LightEntityFeature.EFFECT
    _attr_effect_list = MODES

    @property
    def is_on(self) -> bool | None:
        s = self._device.state
        return s.on if s else None

    @property
    def brightness(self) -> int | None:
        s = self._device.state
        return s.brightness if s else None

    @property
    def rgb_color(self) -> tuple[int, int, int] | None:
        s = self._device.state
        return s.rgb if s else None

    @property
    def effect(self) -> str | None:
        s = self._device.state
        return MODES[s.mode] if s and s.mode < len(MODES) else None

    async def async_turn_on(self, **kwargs: Any) -> None:
        s = self._device.state
        if s is None:
            raise HomeAssistantError("LumenC3 is not connected")
        mode = MODES.index(kwargs[ATTR_EFFECT]) if kwargs.get(ATTR_EFFECT) in MODES else s.mode
        # one SET_ALL frame, so power/colour/brightness/mode change together
        try:
            self._device.set_all(
                True,
                mode,
                kwargs.get(ATTR_RGB_COLOR, s.rgb),
                kwargs.get(ATTR_BRIGHTNESS, s.brightness),
                s.speed,
            )
        except LumenC3Error as err:
            raise HomeAssistantError(str(err)) from err

    async def async_turn_off(self, **kwargs: Any) -> None:
        try:
            self._device.set_power(False)
        except LumenC3Error as err:
            raise HomeAssistantError(str(err)) from err
