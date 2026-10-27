// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 siakinnik
// Wi-Fi station driven by smart-core properties, plus mDNS advertising.
//
// Credentials are LOCAL properties: they can only be set over USB (or
// another trusted transport), never from the network. No access point,
// no captive portal.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Registers sys.wifi_ssid, sys.wifi_pass, sys.wifi, sys.ip, sys.wifi_error. Call after
// smart_esp_init(), before smart_load().
void smart_wifi_register(void);

// Bring Wi-Fi up with the stored credentials and advertise _smart._tcp.
// Call after smart_load().
void smart_wifi_start(const char *hostname_prefix, uint16_t tcp_port, uint16_t model_id, const char *model);

// Call from on_change: reconnects if `id` is one of the credential properties.
// Returns 1 if it was.
int smart_wifi_on_change(uint8_t id);

// Erase the stored network (pairing mode). Call before smart_wifi_start().
void smart_wifi_forget(void);

// Advertise "pair=1" in mDNS while the pairing window is open.
void smart_wifi_set_pairing(bool open);
