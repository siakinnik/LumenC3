// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 siakinnik
// Strip animations. Pure C, no ESP-IDF headers: renders into an RGB buffer.
#pragma once

#include <stdint.h>

#include "smart_core.h"

typedef enum {
    FX_SOLID, FX_RAINBOW, FX_FIRE, FX_COMET, FX_TWINKLE, FX_BREATHE, FX_CHASE, FX_COUNT
} effect_t;

#define EFFECT_NAMES { "solid", "rainbow", "fire", "comet", "twinkle", "breathe", "chase" }

#define FX_MAX_LEDS 300

// Render the next frame at full brightness into `leds`. Keep the same buffer
// between calls: fading effects build on the previous frame. `speed` is 1..100.
void effects_render(smart_rgb_t *leds, int count, effect_t effect, smart_rgb_t color, uint8_t speed, uint32_t now_ms);

// Scale a frame by `brightness` (1..255), apply strip color correction and cap
// the estimated current at `max_ma`, writing the result to `out`.
void effects_output(const smart_rgb_t *in, smart_rgb_t *out, int count, uint8_t brightness, uint32_t max_ma, uint32_t ma_per_channel);
