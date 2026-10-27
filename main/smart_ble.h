// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 siakinnik
// BLE commissioning transport (NimBLE): a GATT service carrying smart-core
// frames, one host at a time. Untrusted like TCP: the host runs the Noise
// handshake and must be paired, or pair in the open pairing window. The
// device only advertises while smart_ble_start() is in effect, i.e. while
// the pairing window is open.
//
// Moves to the smart-esp-device template together with smart_esp.c.
#pragma once

// Bring up the BLE stack and the GATT service; doesn't advertise yet.
void smart_ble_init(const char *device_name);

// Advertise (as `device_name`, with the smart-core service UUID).
void smart_ble_start(void);

// Stop advertising and drop the connected host.
void smart_ble_stop(void);
