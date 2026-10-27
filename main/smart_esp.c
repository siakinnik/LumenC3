// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 siakinnik
#include "smart_esp.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "bootloader_random.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "smart";
#define NVS_NAMESPACE "smart"

smart_core_t g_smart;

static SemaphoreHandle_t core_lock;   // recursive: guards g_smart and links[]
static SemaphoreHandle_t usb_tx_lock; // one writer at a time on USB: packets and log lines
static nvs_handle_t nvs;
static bool rf_on; // Wi-Fi running: esp_fill_random() is a true RNG

static esp_ota_handle_t ota_handle;
static const esp_partition_t *ota_part;
static volatile bool update_ready;

typedef enum
{
    LINK_NONE,
    LINK_USB,
    LINK_TCP,
    LINK_EXT
} link_t;
static struct
{
    link_t link;
    int fd;                      // LINK_TCP
    const smart_esp_link_t *ext; // LINK_EXT (BLE, ...)
    int64_t since;
} links[SMART_MAX_SESSIONS];

// ---------- lock ----------
void smart_lock(void) { xSemaphoreTakeRecursive(core_lock, portMAX_DELAY); }
void smart_unlock(void) { xSemaphoreGiveRecursive(core_lock); }

// ---------- port callbacks ----------
static uint32_t port_millis(void *ctx)
{
    (void)ctx;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static int usb_write(const void *data, size_t len)
{
    xSemaphoreTake(usb_tx_lock, portMAX_DELAY);
    // short timeout: with no host reading, the TX buffer fills and data is dropped
    int n = usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(20));
    xSemaphoreGive(usb_tx_lock);
    return n == (int)len ? 0 : -1;
}

// called with the core lock held (every send comes from inside smart_*)
static int port_send(void *ctx, uint8_t session, const uint8_t *data, size_t len)
{
    (void)ctx;
    if (session >= SMART_MAX_SESSIONS)
        return -1;
    switch (links[session].link)
    {
    case LINK_USB:
        return usb_write(data, len);
    case LINK_TCP:
    {
        size_t done = 0;
        while (done < len)
        {
            int n = send(links[session].fd, data + done, len - done, 0);
            if (n <= 0)
                return -1; // the reader task notices the dead socket and closes it
            done += (size_t)n;
        }
        return 0;
    }
    case LINK_EXT:
        return links[session].ext->send(links[session].ext->arg, data, len);
    default:
        return -1;
    }
}

static int port_store_read(void *ctx, const char *key, void *buf, size_t cap)
{
    (void)ctx;
    size_t len = cap;
    if (nvs_get_blob(nvs, key, buf, &len) != ESP_OK)
        return -1;
    return (int)len;
}

static int port_store_write(void *ctx, const char *key, const void *data, size_t len)
{
    (void)ctx;
    esp_err_t err = nvs_set_blob(nvs, key, data, len);
    if (err == ESP_OK)
        err = nvs_commit(nvs);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "can't save %s: %s", key, esp_err_to_name(err));
    return err == ESP_OK ? 0 : -1;
}

// Keys need real entropy. With the radio running, esp_fill_random() is a
// true RNG; before that (the device key is made at the first boot, ahead of
// Wi-Fi) the bootloader's entropy source has to be switched on for it.
static void port_random(void *ctx, uint8_t *buf, size_t len)
{
    (void)ctx;
    if (rf_on)
    {
        esp_fill_random(buf, len);
        return;
    }
    bootloader_random_enable();
    esp_fill_random(buf, len);
    bootloader_random_disable();
}

// The core gave up on a network session (failed handshake, unpaired host).
// Called with the core lock held.
static void port_close(void *ctx, uint8_t session)
{
    (void)ctx;
    if (session >= SMART_MAX_SESSIONS)
        return;
    if (links[session].link == LINK_TCP)
    {
        close(links[session].fd);
        links[session].fd = -1;
    }
    else if (links[session].link == LINK_EXT)
    {
        links[session].ext->close(links[session].ext->arg);
    }
    else
    {
        return;
    }
    links[session].link = LINK_NONE;
}

// ---------- OTA: the core has checked signature, model and hash; we write ----------
static int port_ota_begin(void *ctx, uint32_t total)
{
    (void)ctx;
    ota_part = esp_ota_get_next_update_partition(NULL);
    if (!ota_part || total > ota_part->size)
    {
        ESP_LOGE(TAG, "update of %lu bytes doesn't fit", (unsigned long)total);
        return -1;
    }
    // erase as we go, not the whole slot up front: no long stall before the first chunk
    esp_err_t err = esp_ota_begin(ota_part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "can't start the update: %s", esp_err_to_name(err));
        ota_handle = 0;
        return -1;
    }
    ESP_LOGI(TAG, "receiving update: %lu bytes into %s", (unsigned long)total, ota_part->label);
    return 0;
}

static int port_ota_write(void *ctx, uint32_t offset, const uint8_t *data, size_t len)
{
    (void)ctx;
    (void)offset; // the core hands bytes over strictly in order
    return ota_handle && esp_ota_write(ota_handle, data, len) == ESP_OK ? 0 : -1;
}

static int port_ota_finish(void *ctx, bool ok)
{
    (void)ctx;
    if (!ota_handle)
        return ok ? -1 : 0;
    esp_ota_handle_t h = ota_handle;
    ota_handle = 0;
    if (!ok)
    {
        esp_ota_abort(h);
        ESP_LOGW(TAG, "update aborted");
        return 0;
    }
    esp_err_t err = esp_ota_end(h); // also checks the app image itself
    if (err == ESP_OK)
        err = esp_ota_set_boot_partition(ota_part);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "can't install the update: %s", esp_err_to_name(err));
        return -1;
    }
    ESP_LOGI(TAG, "update installed in %s, rebooting into it", ota_part->label);
    update_ready = true;
    return 0;
}

bool smart_esp_update_ready(void) { return update_ready; }

static const smart_port_t port = {
    .ctx = NULL,
    .millis = port_millis,
    .send = port_send,
    .store_read = port_store_read,
    .store_write = port_store_write,
    .random = port_random,
    .close = port_close,
    .ota_begin = port_ota_begin,
    .ota_write = port_ota_write,
    .ota_finish = port_ota_finish,
};

const smart_port_t *smart_esp_port(void) { return &port; }

void smart_esp_rf_started(void) { rf_on = true; }

void smart_esp_init(void)
{
    core_lock = xSemaphoreCreateRecursiveMutex();
    usb_tx_lock = xSemaphoreCreateMutex();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs));
}

void smart_esp_erase(void)
{
    nvs_erase_all(nvs);
    nvs_commit(nvs);
}

const char *smart_esp_device_id(void)
{
    static char id[18];
    if (!id[0])
    {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(id, sizeof(id), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    return id;
}

// ---------- USB ----------
// ESP log lines go through the same lock as protocol packets, so a log line
// never lands in the middle of a packet. They contain no zero bytes, so hosts
// skip them (see PROTOCOL.md, Framing).
static int log_vprintf(const char *fmt, va_list args)
{
    char line[256];
    int n = vsnprintf(line, sizeof(line), fmt, args);
    if (n > 0)
        usb_write(line, n < (int)sizeof(line) ? (size_t)n : sizeof(line) - 1);
    return n;
}

static void usb_task(void *arg)
{
    uint8_t session = (uint8_t)(uintptr_t)arg;
    uint8_t buf[64];
    for (;;)
    {
        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), portMAX_DELAY);
        if (n <= 0)
            continue;
        smart_lock();
        smart_session_feed(&g_smart, session, buf, (size_t)n);
        smart_unlock();
    }
}

void smart_esp_start_usb(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.tx_buffer_size = 1024;
    cfg.rx_buffer_size = 512;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    usb_serial_jtag_vfs_use_driver(); // printf() goes through the driver too
    esp_log_set_vprintf(log_vprintf);

    smart_lock();
    uint8_t s = smart_session_open(&g_smart, true); // physical access = trusted
    if (s != SMART_NO_SESSION)
        links[s].link = LINK_USB;
    smart_unlock();
    if (s == SMART_NO_SESSION)
    {
        ESP_LOGE(TAG, "no session left for USB");
        return;
    }
    xTaskCreate(usb_task, "smart_usb", 4096, (void *)(uintptr_t)s, 5, NULL);
}

// ---------- untrusted sessions (TCP, BLE, ...) ----------
// An untrusted session, making room if every one is taken: drop the oldest
// connection that hasn't authenticated. A paired host is never pushed out by
// a stranger; a dead one goes away by itself (TCP keepalive, BLE timeout).
// Call with the core lock held.
static uint8_t open_untrusted(void)
{
    uint8_t s = smart_session_open(&g_smart, false);
    if (s == SMART_NO_SESSION)
    {
        int oldest = -1;
        for (int i = 0; i < SMART_MAX_SESSIONS; i++)
            if ((links[i].link == LINK_TCP || links[i].link == LINK_EXT) &&
                !smart_session_authenticated(&g_smart, (uint8_t)i) &&
                (oldest < 0 || links[i].since < links[oldest].since))
                oldest = i;
        if (oldest >= 0)
        {
            ESP_LOGI(TAG, "dropping an unauthenticated connection to make room");
            smart_session_close(&g_smart, (uint8_t)oldest);
            port_close(NULL, (uint8_t)oldest);
            s = smart_session_open(&g_smart, false);
        }
    }
    if (s != SMART_NO_SESSION)
        links[s].since = esp_timer_get_time();
    return s;
}

uint8_t smart_esp_link_open(const smart_esp_link_t *link)
{
    smart_lock();
    uint8_t s = open_untrusted();
    if (s != SMART_NO_SESSION)
    {
        links[s].link = LINK_EXT;
        links[s].ext = link;
    }
    smart_unlock();
    return s;
}

void smart_esp_link_feed(uint8_t s, const uint8_t *data, size_t len)
{
    smart_lock();
    if (s < SMART_MAX_SESSIONS && links[s].link == LINK_EXT)
        smart_session_feed(&g_smart, s, data, len);
    smart_unlock();
}

void smart_esp_link_gone(uint8_t s)
{
    smart_lock();
    if (s < SMART_MAX_SESSIONS && links[s].link == LINK_EXT)
    {
        smart_session_close(&g_smart, s);
        links[s].link = LINK_NONE;
    }
    smart_unlock();
}

// ---------- TCP ----------
static void tcp_close_session(uint8_t s)
{
    smart_lock();
    smart_session_close(&g_smart, s);
    close(links[s].fd);
    links[s].link = LINK_NONE;
    links[s].fd = -1;
    smart_unlock();
}

static void tcp_accept(int lfd)
{
    struct sockaddr_in6 peer;
    socklen_t plen = sizeof(peer);
    int fd = accept(lfd, (struct sockaddr *)&peer, &plen);
    if (fd < 0)
        return;

    int one = 1, idle = 30, intvl = 5, cnt = 3;
    struct timeval tmo = {.tv_sec = 0, .tv_usec = 300000};
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    // a host that vanished without closing (power cut) is noticed in ~45 s
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
    // a host that stops reading must not stall the device
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tmo, sizeof(tmo));

    smart_lock();
    uint8_t s = open_untrusted();
    if (s != SMART_NO_SESSION)
    {
        links[s].link = LINK_TCP;
        links[s].fd = fd;
    }
    smart_unlock();
    if (s == SMART_NO_SESSION)
        close(fd);
}

static void tcp_task(void *arg)
{
    uint16_t port = (uint16_t)(uintptr_t)arg;
    int lfd = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP); // dual-stack: IPv4 too
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in6 addr = {.sin6_family = AF_INET6, .sin6_port = htons(port)};
    if (lfd < 0 || bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) || listen(lfd, 2))
    {
        ESP_LOGE(TAG, "TCP server failed: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "listening on TCP %u", port);

    for (;;)
    {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(lfd, &rd);
        int maxfd = lfd;
        int fds[SMART_MAX_SESSIONS];
        smart_lock();
        for (int i = 0; i < SMART_MAX_SESSIONS; i++)
        {
            fds[i] = links[i].link == LINK_TCP ? links[i].fd : -1;
            if (fds[i] >= 0)
            {
                FD_SET(fds[i], &rd);
                if (fds[i] > maxfd)
                    maxfd = fds[i];
            }
        }
        smart_unlock();

        struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
        if (select(maxfd + 1, &rd, NULL, NULL, &tv) <= 0)
            continue;

        // Read first, accept last: a socket the core closes meanwhile may hand
        // its number to the next accept(), which must not happen inside this loop.
        for (int i = 0; i < SMART_MAX_SESSIONS; i++)
        {
            if (fds[i] < 0 || !FD_ISSET(fds[i], &rd))
                continue;
            uint8_t buf[256];
            int n = recv(fds[i], buf, sizeof(buf), 0);
            if (n <= 0)
            {
                if (links[i].link == LINK_TCP && links[i].fd == fds[i])
                    tcp_close_session((uint8_t)i);
                continue;
            }
            smart_lock();
            if (links[i].link == LINK_TCP && links[i].fd == fds[i]) // not replaced meanwhile
                smart_session_feed(&g_smart, (uint8_t)i, buf, (size_t)n);
            smart_unlock();
        }

        if (FD_ISSET(lfd, &rd))
            tcp_accept(lfd);
    }
}

void smart_esp_start_tcp(uint16_t port)
{
    for (int i = 0; i < SMART_MAX_SESSIONS; i++)
        if (links[i].link == LINK_NONE)
            links[i].fd = -1;
    // the Noise handshake (X25519) runs in this task: give it room
    xTaskCreate(tcp_task, "smart_tcp", 8192, (void *)(uintptr_t)port, 5, NULL);
}
