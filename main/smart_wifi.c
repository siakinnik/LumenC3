// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 siakinnik
#include "smart_wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "smart_esp.h"

static const char *TAG = "wifi";

static const char *const STATES[] = { "unconfigured", "connecting", "connected" };
enum { ST_UNCONFIGURED, ST_CONNECTING, ST_CONNECTED };

// Why the last attempt failed, so a host can tell a typo from a router that's off.
static const char *const ERRORS[] = { "none", "wrong_password", "no_network", "failed" };
enum { ERR_NONE, ERR_WRONG_PASSWORD, ERR_NO_NETWORK, ERR_FAILED };

static uint8_t P_SSID, P_PASS, P_STATE, P_IP, P_ERROR;
static char ssid[33], pass[65], ip[16];

static esp_netif_t *netif;
static bool mdns_up;
static esp_timer_handle_t retry_timer;
static uint32_t backoff_ms = 1000;

static void set_state(uint8_t state, const char *addr, uint8_t error)
{
    smart_lock();
    smart_set_u8(&g_smart, P_ERROR, error);  // before the state: a host waiting on it sees why
    smart_set_u8(&g_smart, P_STATE, state);
    smart_set_str(&g_smart, P_IP, addr);
    smart_unlock();
}

static uint8_t error_for(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_ASSOC_LEAVE:  // we left on purpose (new credentials, reboot)
        return ERR_NONE;
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        return ERR_WRONG_PASSWORD;
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return ERR_NO_NETWORK;
    default:
        return ERR_FAILED;
    }
}

static bool configured(void)
{
    smart_lock();
    bool yes = smart_get_str(&g_smart, P_SSID)[0] != 0;
    smart_unlock();
    return yes;
}

static void schedule_connect(uint32_t delay_ms)
{
    esp_timer_stop(retry_timer);
    esp_timer_start_once(retry_timer, (uint64_t)delay_ms * 1000);
}

static void retry_cb(void *arg)
{
    (void)arg;
    if (configured())
        esp_wifi_connect();
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!configured()) {
            set_state(ST_UNCONFIGURED, "", ERR_NONE);
            return;
        }
        wifi_event_sta_disconnected_t *ev = data;
        ESP_LOGW(TAG, "disconnected (reason %d), retry in %lu ms", ev->reason, (unsigned long)backoff_ms);
        set_state(ST_CONNECTING, "", error_for(ev->reason));
        schedule_connect(backoff_ms);
        backoff_ms = backoff_ms * 2 > 30000 ? 30000 : backoff_ms * 2;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        char addr[16];
        snprintf(addr, sizeof(addr), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "connected: %s", addr);
        backoff_ms = 1000;
        set_state(ST_CONNECTED, addr, ERR_NONE);
    }
}

// (Re)apply the credentials from the properties.
static void apply(void)
{
    wifi_config_t cfg = { 0 };
    smart_lock();
    strncpy((char *)cfg.sta.ssid, smart_get_str(&g_smart, P_SSID), sizeof(cfg.sta.ssid));
    strncpy((char *)cfg.sta.password, smart_get_str(&g_smart, P_PASS), sizeof(cfg.sta.password));
    smart_unlock();
    cfg.sta.threshold.authmode = cfg.sta.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;

    esp_timer_stop(retry_timer);
    esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    backoff_ms = 1000;

    if (!cfg.sta.ssid[0]) {
        ESP_LOGI(TAG, "no network configured; set sys.wifi_ssid over USB");
        set_state(ST_UNCONFIGURED, "", ERR_NONE);
        return;
    }
    ESP_LOGI(TAG, "connecting to \"%s\"", (char *)cfg.sta.ssid);
    set_state(ST_CONNECTING, "", ERR_NONE);
    schedule_connect(100);
}

// Firmware v1 (Arduino) let the Wi-Fi driver keep the network in its own NVS
// namespace. Take it over once, then wipe the driver's copy: from now on the
// credentials live only in the smart-core properties. Call right after
// esp_wifi_init(), while the driver still uses flash storage.
static void import_legacy_credentials(void)
{
    wifi_config_t old = { 0 };
    if (esp_wifi_get_config(WIFI_IF_STA, &old) != ESP_OK || !old.sta.ssid[0])
        return;
    if (!configured()) {
        char s[sizeof(old.sta.ssid) + 1] = { 0 }, p[sizeof(old.sta.password) + 1] = { 0 };
        memcpy(s, old.sta.ssid, sizeof(old.sta.ssid));
        memcpy(p, old.sta.password, sizeof(old.sta.password));
        smart_lock();
        bool ok = smart_set_str(&g_smart, P_SSID, s) && smart_set_str(&g_smart, P_PASS, p);
        smart_flush(&g_smart);
        smart_unlock();
        if (ok)
            ESP_LOGI(TAG, "took over Wi-Fi \"%s\" saved by the previous firmware", s);
    }
    esp_wifi_restore();  // erases the driver's stored config, password included
}

void smart_wifi_register(void)
{
    P_SSID = smart_prop_str(&g_smart, "sys.wifi_ssid", SMART_F_RW | SMART_F_LOCAL | SMART_F_PERSIST, ssid, 32);
    P_PASS = smart_prop_str(&g_smart, "sys.wifi_pass", SMART_F_WRITE | SMART_F_SECRET | SMART_F_LOCAL | SMART_F_PERSIST, pass, 64);
    P_STATE = smart_prop_enum(&g_smart, "sys.wifi", SMART_F_READ, STATES, 3, ST_UNCONFIGURED);
    P_IP = smart_prop_str(&g_smart, "sys.ip", SMART_F_READ, ip, 15);
    // appended later: ids above never move
    P_ERROR = smart_prop_enum(&g_smart, "sys.wifi_error", SMART_F_READ, ERRORS, 4, ERR_NONE);
}

int smart_wifi_on_change(uint8_t id)
{
    if (id != P_SSID && id != P_PASS)
        return 0;
    smart_flush(&g_smart);  // credentials are saved right away, not 3 s later
    apply();
    return 1;
}

void smart_wifi_start(const char *hostname_prefix, uint16_t tcp_port, uint16_t model_id, const char *model)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    netif = esp_netif_create_default_wifi_sta();

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    static char hostname[32];
    snprintf(hostname, sizeof(hostname), "%s-%02x%02x", hostname_prefix, mac[4], mac[5]);
    esp_netif_set_hostname(netif, hostname);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    import_legacy_credentials();
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));  // credentials live in smart-core's NVS
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL));

    const esp_timer_create_args_t targs = { .callback = retry_cb, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &retry_timer));

    ESP_ERROR_CHECK(esp_wifi_start());
    smart_esp_rf_started();
    apply();

    // mDNS: hosts find the device as _smart._tcp, see PROTOCOL.md (Discovery)
    static char model_id_str[8];
    snprintf(model_id_str, sizeof(model_id_str), "%u", model_id);
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(hostname);
        mdns_txt_item_t txt[] = {
            { "id", smart_esp_device_id() },
            { "model", model },
            { "model_id", model_id_str },
            { "proto", "1" },
        };
        mdns_service_add(hostname, "_smart", "_tcp", tcp_port, txt, sizeof(txt) / sizeof(txt[0]));
        mdns_up = true;
    }
    ESP_LOGI(TAG, "hostname %s.local", hostname);
}

void smart_wifi_forget(void)
{
    smart_lock();
    smart_set_str(&g_smart, P_SSID, "");
    smart_set_str(&g_smart, P_PASS, "");
    smart_flush(&g_smart);
    smart_unlock();
}

void smart_wifi_set_pairing(bool open)
{
    if (!mdns_up)
        return;
    if (open)
        mdns_service_txt_item_set("_smart", "_tcp", "pair", "1");
    else
        mdns_service_txt_item_remove("_smart", "_tcp", "pair");
}
