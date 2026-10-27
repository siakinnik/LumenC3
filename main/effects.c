// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 siakinnik
// Strip animations, ported from the FastLED version of LumenC3.
#include "effects.h"

#include <string.h>

// ---------- small FastLED-style helpers ----------
static uint32_t rng = 0x12345678;

static uint8_t random8(void)
{
    rng ^= rng << 13;  // xorshift32
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return (uint8_t)(rng >> 24);
}
static uint8_t random8_range(uint8_t lo, uint8_t hi) { return (uint8_t)(lo + (random8() * (hi - lo)) / 256); }
static uint16_t random16(uint16_t lim) { return (uint16_t)(((random8() << 8 | random8()) * (uint32_t)lim) >> 16); }

static uint8_t scale8(uint8_t v, uint8_t scale) { return (uint8_t)((v * (1 + scale)) >> 8); }
static uint8_t qadd8(uint8_t a, uint8_t b) { return (uint8_t)(a + b > 255 ? 255 : a + b); }
static uint8_t qsub8(uint8_t a, uint8_t b) { return (uint8_t)(a > b ? a - b : 0); }

static smart_rgb_t scale_rgb(smart_rgb_t c, uint8_t scale)
{
    smart_rgb_t o = { scale8(c.r, scale), scale8(c.g, scale), scale8(c.b, scale) };
    return o;
}

static void fill(smart_rgb_t *leds, int count, smart_rgb_t c)
{
    for (int i = 0; i < count; i++)
        leds[i] = c;
}

static void fade_to_black(smart_rgb_t *leds, int count, uint8_t amount)
{
    for (int i = 0; i < count; i++)
        leds[i] = scale_rgb(leds[i], (uint8_t)(255 - amount));
}

// hue 0..255 around the color wheel, full saturation and value
static smart_rgb_t hue_to_rgb(uint8_t hue)
{
    uint8_t sector = hue / 43, rem = (uint8_t)((hue - sector * 43) * 6);
    uint8_t up = rem, down = (uint8_t)(255 - rem);
    switch (sector) {
    case 0: return (smart_rgb_t){ 255, up, 0 };
    case 1: return (smart_rgb_t){ down, 255, 0 };
    case 2: return (smart_rgb_t){ 0, 255, up };
    case 3: return (smart_rgb_t){ 0, down, 255 };
    case 4: return (smart_rgb_t){ up, 0, 255 };
    default: return (smart_rgb_t){ 255, 0, down };
    }
}

// Fire2012 palette: black -> red -> yellow -> white
static smart_rgb_t heat_color(uint8_t temperature)
{
    uint8_t t192 = scale8(temperature, 191);
    uint8_t ramp = (uint8_t)((t192 & 0x3F) << 2);
    if (t192 & 0x80)
        return (smart_rgb_t){ 255, 255, ramp };
    if (t192 & 0x40)
        return (smart_rgb_t){ 255, ramp, 0 };
    return (smart_rgb_t){ ramp, 0, 0 };
}

// ---------- effects ----------
void effects_render(smart_rgb_t *leds, int count, effect_t effect, smart_rgb_t color, uint8_t speed, uint32_t now_ms)
{
    static uint8_t hue;
    static uint8_t heat[FX_MAX_LEDS];
    static uint16_t comet_pos;
    static uint8_t breath = 5;
    static int breath_up = 1;
    static uint8_t chase_step;
    static uint32_t chase_last;

    if (count > FX_MAX_LEDS)
        count = FX_MAX_LEDS;
    rng ^= now_ms;  // a little extra entropy, harmless for animations

    switch (effect) {
    case FX_SOLID:
        fill(leds, count, color);
        break;

    case FX_RAINBOW: {
        uint8_t delta = (uint8_t)(256 / count + 1);
        for (int i = 0; i < count; i++)
            leds[i] = hue_to_rgb((uint8_t)(hue + i * delta));
        hue = (uint8_t)(hue + speed / 12 + 1);
        break;
    }

    case FX_FIRE: {  // Fire2012: cooling 55, sparking 120
        const uint8_t cooling = 55, sparking = 120;
        for (int i = 0; i < count; i++)
            heat[i] = qsub8(heat[i], random8_range(0, (uint8_t)((cooling * 10) / count + 2)));
        for (int k = count - 1; k >= 2; k--)
            heat[k] = (uint8_t)((heat[k - 1] + heat[k - 2] + heat[k - 2]) / 3);
        if (random8() < sparking) {
            int y = random8_range(0, 7);
            if (y < count)
                heat[y] = qadd8(heat[y], random8_range(160, 255));
        }
        for (int j = 0; j < count; j++)
            leds[j] = heat_color(heat[j]);
        break;
    }

    case FX_COMET:
        fade_to_black(leds, count, 40);
        comet_pos = (uint16_t)(comet_pos + speed / 10 + 1);
        leds[(comet_pos / 4) % count] = color;
        break;

    case FX_TWINKLE:
        fade_to_black(leds, count, 24);
        if (random8() < speed + 20)
            leds[random16((uint16_t)count)] = color;
        break;

    case FX_BREATHE:
        breath = (uint8_t)(breath + (breath_up ? 3 : -3));
        if (breath >= 250)
            breath_up = 0;
        if (breath <= 5)
            breath_up = 1;
        fill(leds, count, scale_rgb(color, breath));
        break;

    case FX_CHASE: {
        smart_rgb_t black = { 0, 0, 0 };
        fill(leds, count, black);
        for (int i = chase_step % 3; i < count; i += 3)
            leds[i] = color;
        if (now_ms - chase_last > (uint32_t)(400 - speed * 3)) {
            chase_step++;
            chase_last = now_ms;
        }
        break;
    }

    default:
        break;
    }
}

void effects_output(const smart_rgb_t *in, smart_rgb_t *out, int count, uint8_t brightness, uint32_t max_ma, uint32_t ma_per_channel)
{
    // brightness, then the "typical LED strip" correction FastLED used (FF B0 F0)
    uint32_t sum = 0;
    for (int i = 0; i < count; i++) {
        smart_rgb_t c = scale_rgb(in[i], brightness);
        c.g = scale8(c.g, 0xB0);
        c.b = scale8(c.b, 0xF0);
        out[i] = c;
        sum += (uint32_t)c.r + c.g + c.b;
    }
    // estimated current: each channel draws ma_per_channel at 255
    uint32_t ma = sum * ma_per_channel / 255;
    if (max_ma && ma > max_ma) {
        uint32_t scale = max_ma * 255 / ma;  // < 255
        for (int i = 0; i < count; i++)
            out[i] = scale_rgb(out[i], (uint8_t)scale);
    }
}
