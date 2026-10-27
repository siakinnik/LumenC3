// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 siakinnik
// Hardware and device settings, all in one place.
#pragma once

// ================== STRIP ==================
#define LED_GPIO            4      // strip data pin
#define LED_COUNT           30     // number of LEDs
#define LED_MODEL           LED_MODEL_WS2812                  // WS2812 / SK6812 / WS2811
#define LED_FORMAT          LED_STRIP_COLOR_COMPONENT_FMT_GRB // swap to _RGB if colors look wrong
#define LED_MAX_MILLIAMPS   1000   // PSU limit: brightness is scaled down to stay under it
#define LED_MA_PER_CHANNEL  20     // current of one fully-lit color channel

// ================== EFFECTS ==================
#define FRAME_MS            16     // one frame every N ms (~60 FPS)
#define DEF_BRIGHTNESS      160
#define DEF_SPEED           50

// ================== PAIRING (no button: the power switch is the button) ==================
#define POWER_CYCLES_PAIR   5      // switch on this many times in a row: pairing mode (forgets Wi-Fi
                                   // and paired hosts, BLE + pairing window)
#define POWER_CYCLES_RESET  10     // ... this many: factory reset
#define POWER_CYCLE_MS      5000   // "in a row" = each time switched off again before this
#define PAIRING_WINDOW_MS   180000 // how long the pairing window stays open

// ================== DEVICE ==================
#define DEVICE_MODEL_ID     0x0001     // siakinnik-home model database
#define DEVICE_MODEL        "lumenc3"
#define DEVICE_HOSTNAME     "lumenc3"  // mDNS: lumenc3-xxxx.local (last MAC bytes)
#define DEVICE_BLE_NAME     "LumenC3"  // BLE while pairing: LumenC3-XXXX
#define SMART_TCP_PORT      7531
