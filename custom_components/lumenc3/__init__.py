"""LumenC3 addressable LED strip controller, connected over USB serial."""

from __future__ import annotations

import asyncio

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import Platform
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import ConfigEntryNotReady

from .const import CONF_PORT, DOMAIN, SETUP_TIMEOUT
from .device import LumenC3Device

PLATFORMS = [Platform.LIGHT, Platform.NUMBER]

type LumenC3ConfigEntry = ConfigEntry[LumenC3Device]


async def async_setup_entry(hass: HomeAssistant, entry: LumenC3ConfigEntry) -> bool:
    device = LumenC3Device(entry.data[CONF_PORT])

    first_state = asyncio.Event()

    def _on_update() -> None:
        if device.state is not None:
            first_state.set()

    remove_listener = device.add_listener(_on_update)
    task = entry.async_create_background_task(hass, device.run(), f"{DOMAIN} {device.port}")
    try:
        await asyncio.wait_for(first_state.wait(), SETUP_TIMEOUT)
    except TimeoutError as err:
        task.cancel()
        raise ConfigEntryNotReady(f"No answer from LumenC3 on {device.port}") from err
    finally:
        remove_listener()

    entry.runtime_data = device
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    return True


async def async_unload_entry(hass: HomeAssistant, entry: LumenC3ConfigEntry) -> bool:
    # the background task (device.run) is cancelled by HA on unload and closes the port
    return await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
