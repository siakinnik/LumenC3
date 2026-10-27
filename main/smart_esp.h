// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 siakinnik
// ESP-IDF port of smart-core: time, NVS storage, a lock, and the transports
// (native USB as the trusted session, TCP for network hosts).
//
// Moves to the smart-esp-device template once a second device uses it.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "smart_core.h"

// The device's core instance (defined in smart_esp.c).
extern smart_core_t g_smart;

// Port callbacks for smart_config_t.port. Call smart_esp_init() first.
const smart_port_t *smart_esp_port(void);

// NVS + the core lock. Call before registering properties.
void smart_esp_init(void);

// Every smart_* call goes between these (recursive: on_change may call smart_set_*).
void smart_lock(void);
void smart_unlock(void);

// Native USB (USB Serial/JTAG) as a trusted session. Also routes the ESP log
// through the same port without splitting protocol packets.
void smart_esp_start_usb(void);

// TCP server for network hosts (untrusted sessions: Noise handshake, paired
// hosts only).
void smart_esp_start_tcp(uint16_t port);

// Other untrusted transports (BLE, a shared bus, ...) plug in here. The
// transport calls smart_esp_link_open() when a host connects, feeds what it
// receives, and smart_esp_link_gone() when the host disconnects. `send` and
// `close` are called with the core lock held; `close` must drop the host
// (and may later report it with smart_esp_link_gone(), which is then a no-op).
typedef struct {
    int (*send)(void *arg, const uint8_t *data, size_t len);  // 0 on success
    void (*close)(void *arg);
    void *arg;
} smart_esp_link_t;

uint8_t smart_esp_link_open(const smart_esp_link_t *link);  // SMART_NO_SESSION if no room
void smart_esp_link_feed(uint8_t session, const uint8_t *data, size_t len);
void smart_esp_link_gone(uint8_t session);

// Tell the port the radio is running: from now on esp_fill_random() is a
// true RNG and needs no help from the bootloader's entropy source.
void smart_esp_rf_started(void);

// "AA:BB:CC:DD:EE:FF" of the Wi-Fi station interface.
const char *smart_esp_device_id(void);

// Erase everything smart-core stored (properties, Wi-Fi credentials).
void smart_esp_erase(void);
