// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 siakinnik
// LumenC3: addressable LED strip on an ESP32-C3 SuperMini, controlled
// through smart-core over USB and Wi-Fi. Settings are in config.h.
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "effects.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "nvs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "smart_ble.h"
#include "smart_core.h"
#include "smart_esp.h"
#include "smart_wifi.h"

static const char *TAG = "lumenc3";

static const char *const EFFECTS[] = EFFECT_NAMES;
_Static_assert(sizeof(EFFECTS) / sizeof(EFFECTS[0]) == FX_COUNT, "EFFECT_NAMES must match effect_t");

// Property ids. Order is part of the model: only ever append new ones.
static uint8_t P_POWER, P_BRIGHTNESS, P_COLOR, P_EFFECT, P_SPEED, P_REBOOT, P_RESET;

static volatile enum { RUN, REBOOT, RESET } pending = RUN;

// What the strip shows on top of the effect while pairing.
static volatile enum { SHOW_NONE, SHOW_PAIRING, SHOW_PAIRED, SHOW_CONFLICT } show = SHOW_NONE;
static volatile uint32_t show_since;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static bool ble_up;  // the BLE stack is only brought up for a pairing window

// Called by the core (with its lock held) on pairing window events.
static void on_pairing(smart_core_t *core, smart_pairing_event_t ev, void *user)
{
    (void)core;
    (void)user;
    static const char *const names[] = { "window open", "paired", "conflict: nobody paired",
                                         "window closed, nobody paired", "window closed" };
    ESP_LOGI(TAG, "pairing: %s", names[ev]);
    if (ev != SMART_PAIRING_PAIRED) {
        bool open = ev == SMART_PAIRING_OPENED;
        smart_wifi_set_pairing(open);
        if (ble_up) {
            if (open)
                smart_ble_start();
            else
                smart_ble_stop();
        }
    }
    show_since = now_ms();
    switch (ev) {
    case SMART_PAIRING_OPENED: show = SHOW_PAIRING; break;
    case SMART_PAIRING_PAIRED: show = SHOW_PAIRED; break;
    case SMART_PAIRING_CONFLICT: show = SHOW_CONFLICT; break;
    default: show = SHOW_NONE; break;
    }
}

// Pairing indication. Returns true if it painted the frame.
//   window open: the whole strip breathes blue
//   paired:      two green flashes
//   conflict:    three red flashes
static bool render_pairing(smart_rgb_t *leds, int count, uint32_t now)
{
    uint32_t t = now - show_since;
    smart_rgb_t c = { 0, 0, 0 };
    switch (show) {
    case SHOW_PAIRING: {
        uint32_t phase = t % 2000;  // 2 s breathing cycle
        uint8_t level = (uint8_t)(phase < 1000 ? 20 + phase * 235 / 1000 : 20 + (2000 - phase) * 235 / 1000);
        c.b = level;
        break;
    }
    case SHOW_PAIRED:
    case SHOW_CONFLICT: {
        uint32_t flashes = show == SHOW_PAIRED ? 2 : 3;
        if (t >= flashes * 500) {
            show = SHOW_NONE;
            return false;
        }
        if (t % 500 < 250) {
            if (show == SHOW_PAIRED)
                c.g = 255;
            else
                c.r = 255;
        }
        break;
    }
    default:
        return false;
    }
    for (int i = 0; i < count; i++)
        leds[i] = c;
    return true;
}

// The strip has no button, so its power switch is one: switching it on
// POWER_CYCLES_PAIR times in a row (each time off again within
// POWER_CYCLE_MS) opens the pairing window, POWER_CYCLES_RESET times resets
// it. Only real power-ons count, so a crash loop can't trigger either.
static nvs_handle_t power_nvs;

static uint8_t count_power_cycle(void)
{
    uint8_t n = 0;
    if (nvs_open("power", NVS_READWRITE, &power_nvs) != ESP_OK)
        return 0;
    if (esp_reset_reason() != ESP_RST_POWERON) {
        nvs_set_u8(power_nvs, "cycles", 0);
        nvs_commit(power_nvs);
        return 0;
    }
    nvs_get_u8(power_nvs, "cycles", &n);
    if (n < 255)
        n++;
    nvs_set_u8(power_nvs, "cycles", n);
    nvs_commit(power_nvs);  // before the user can switch it off again
    return n;
}

static void forget_power_cycles(void)
{
    nvs_set_u8(power_nvs, "cycles", 0);
    nvs_commit(power_nvs);
}

static void on_change(smart_core_t *core, const uint8_t *ids, uint8_t count, void *user)
{
    (void)core;
    (void)user;
    for (uint8_t i = 0; i < count; i++) {
        if (smart_wifi_on_change(ids[i]))
            continue;
        if (ids[i] == P_REBOOT)
            pending = REBOOT;
        else if (ids[i] == P_RESET)
            pending = RESET;
    }
}

static void register_props(void)
{
    smart_rgb_t orange = { 255, 60, 0 };
    P_POWER = smart_prop_bool(&g_smart, "light.power", SMART_F_RW | SMART_F_PERSIST, true);
    P_BRIGHTNESS = smart_prop_u8(&g_smart, "light.brightness", SMART_F_RW | SMART_F_PERSIST, 1, 255, DEF_BRIGHTNESS);
    P_COLOR = smart_prop_rgb(&g_smart, "light.color", SMART_F_RW | SMART_F_PERSIST, orange);
    P_EFFECT = smart_prop_enum(&g_smart, "light.effect", SMART_F_RW | SMART_F_PERSIST, EFFECTS, FX_COUNT, FX_RAINBOW);
    P_SPEED = smart_prop_u8(&g_smart, "light.speed", SMART_F_RW | SMART_F_PERSIST, 1, 100, DEF_SPEED);
    P_REBOOT = smart_prop_action(&g_smart, "sys.reboot", 0);
    P_RESET = smart_prop_action(&g_smart, "sys.reset", SMART_F_LOCAL);  // only over USB
    smart_wifi_register();
}

static led_strip_handle_t strip_init(void)
{
    led_strip_config_t cfg = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = LED_COUNT,
        .led_model = LED_MODEL,
        .color_component_format = LED_FORMAT,
    };
    led_strip_rmt_config_t rmt = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    led_strip_handle_t strip;
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&cfg, &rmt, &strip));
    led_strip_clear(strip);
    return strip;
}

static void restart(bool erase)
{
    smart_lock();
    smart_flush(&g_smart);
    smart_unlock();
    vTaskDelay(pdMS_TO_TICKS(300));  // let the ACK leave over USB / TCP
    if (erase) {
        ESP_LOGW(TAG, "factory reset");
        smart_esp_erase();
    }
    esp_restart();
}

void app_main(void)
{
    smart_esp_init();

    smart_config_t cfg = {
        .port = smart_esp_port(),
        .device_id = smart_esp_device_id(),
        .model_id = DEVICE_MODEL_ID,
        .model = DEVICE_MODEL,
        .firmware = esp_app_get_description()->version,
        .on_change = on_change,
        .on_pairing = on_pairing,
    };
    smart_init(&g_smart, &cfg);
    register_props();
    smart_load(&g_smart);

    smart_esp_start_usb();
    ESP_LOGI(TAG, "starting, firmware %s, id %s", cfg.firmware, cfg.device_id);

    uint8_t cycles = count_power_cycle();
    if (cycles >= POWER_CYCLES_RESET) {
        ESP_LOGW(TAG, "switched on %u times in a row: factory reset", cycles);
        forget_power_cycles();
        restart(true);
    }
    if (cycles > 1)
        ESP_LOGI(TAG, "power cycle %u of %d (pairing) / %d (reset)", cycles, POWER_CYCLES_PAIR, POWER_CYCLES_RESET);

    led_strip_handle_t strip = strip_init();
    // Pairing mode: start over as a new device on a new network. Forget the
    // Wi-Fi and every paired host, then let one host pair and commission the
    // strip over BLE (or over Wi-Fi, if it gets a network meanwhile).
    bool pairing_mode = cycles == POWER_CYCLES_PAIR;
    if (pairing_mode) {
        ESP_LOGW(TAG, "pairing mode: forgetting Wi-Fi and paired hosts");
        smart_lock();
        smart_unpair_all(&g_smart);
        smart_unlock();
        smart_wifi_forget();
    }

    smart_wifi_start(DEVICE_HOSTNAME, SMART_TCP_PORT, DEVICE_MODEL_ID, DEVICE_MODEL);
    smart_esp_start_tcp(SMART_TCP_PORT);
    if (pairing_mode) {
        char ble_name[24];
        const char *id = cfg.device_id;  // "8C:D0:B2:A8:62:B2": name it after the last two bytes
        snprintf(ble_name, sizeof(ble_name), "%s-%c%c%c%c", DEVICE_BLE_NAME, id[12], id[13], id[15], id[16]);
        smart_ble_init(ble_name);
        ble_up = true;
        smart_lock();
        smart_pairing_open(&g_smart, PAIRING_WINDOW_MS);
        smart_unlock();
    }
    bool cycles_forgotten = cycles == 0;

    // we got this far: keep this firmware if it came as an OTA update
    esp_ota_mark_app_valid_cancel_rollback();

    static smart_rgb_t frame[LED_COUNT], overlay[LED_COUNT], out[LED_COUNT];
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(FRAME_MS));
        if (pending != RUN)
            restart(pending == RESET);
        uint32_t now = now_ms();
        if (!cycles_forgotten && now >= POWER_CYCLE_MS) {
            forget_power_cycles();  // stayed on long enough: the next power-on starts from 1
            cycles_forgotten = true;
        }

        smart_lock();
        bool on = smart_get_bool(&g_smart, P_POWER);
        uint8_t brightness = smart_get_u8(&g_smart, P_BRIGHTNESS);
        smart_rgb_t color = smart_get_rgb(&g_smart, P_COLOR);
        effect_t effect = (effect_t)smart_get_u8(&g_smart, P_EFFECT);
        uint8_t speed = smart_get_u8(&g_smart, P_SPEED);
        smart_poll(&g_smart);
        smart_unlock();

        effects_render(frame, LED_COUNT, effect, color, speed, now);
        if (render_pairing(overlay, LED_COUNT, now))  // shown even when the light is off
            effects_output(overlay, out, LED_COUNT, 255, LED_MAX_MILLIAMPS, LED_MA_PER_CHANNEL);
        else if (on)
            effects_output(frame, out, LED_COUNT, brightness, LED_MAX_MILLIAMPS, LED_MA_PER_CHANNEL);
        else
            memset(out, 0, sizeof(out));

        for (int i = 0; i < LED_COUNT; i++)
            led_strip_set_pixel(strip, i, out[i].r, out[i].g, out[i].b);
        led_strip_refresh(strip);
    }
}
