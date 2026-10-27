// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 siakinnik
#include "smart_ble.h"

#include <string.h>

#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "smart_esp.h"

static const char *TAG = "ble";

// smart-core over BLE (UUIDs as in PROTOCOL.md; byte order reversed here):
//   service 5ec7a3e0-6b1d-4b8e-9d2a-4c1e3f7a0001
//   RX      ...0002  write: host -> device frames
//   TX      ...0003  notify: device -> host frames
#define SMART_UUID(last) \
    BLE_UUID128_INIT(last, 0x00, 0x7a, 0x3f, 0x1e, 0x4c, 0x2a, 0x9d, 0x8e, 0x4b, 0x1d, 0x6b, 0xe0, 0xa3, 0xc7, 0x5e)
static const ble_uuid128_t SVC_UUID = SMART_UUID(0x01);
static const ble_uuid128_t RX_UUID = SMART_UUID(0x02);
static const ble_uuid128_t TX_UUID = SMART_UUID(0x03);

static char name[24];
static uint8_t own_addr_type;
static bool synced, want_adv;
static uint16_t tx_handle;
static uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t mtu = 23;
static uint8_t session = SMART_NO_SESSION;

static void advertise(void);

// ---------- link to smart-core ----------
// Called with the core lock held. A frame goes out as notifications of at
// most MTU - 3 bytes; the host's FrameParser glues them back together.
static int ble_send(void *arg, const uint8_t *data, size_t len)
{
    (void)arg;
    uint16_t c = conn;
    if (c == BLE_HS_CONN_HANDLE_NONE)
        return -1;
    size_t chunk = mtu > 3 ? (size_t)mtu - 3 : 20;
    for (size_t off = 0; off < len; off += chunk) {
        size_t n = len - off < chunk ? len - off : chunk;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data + off, (uint16_t)n);
        if (!om || ble_gatts_notify_custom(c, tx_handle, om) != 0)
            return -1;
    }
    return 0;
}

static void ble_drop(void *arg)
{
    (void)arg;
    if (conn != BLE_HS_CONN_HANDLE_NONE)
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
}

static const smart_esp_link_t LINK = { .send = ble_send, .close = ble_drop, .arg = NULL };

// ---------- GATT ----------
static int access_cb(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
        return BLE_ATT_ERR_UNLIKELY;
    uint8_t buf[512];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (session != SMART_NO_SESSION)
        smart_esp_link_feed(session, buf, len);
    return 0;
}

static const struct ble_gatt_svc_def SERVICES[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = &RX_UUID.u, .access_cb = access_cb, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = &TX_UUID.u, .access_cb = access_cb, .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &tx_handle },
            { 0 },
        },
    },
    { 0 },
};

// ---------- GAP ----------
static int gap_event(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status != 0) {
            advertise();
            break;
        }
        conn = ev->connect.conn_handle;
        mtu = 23;
        ESP_LOGI(TAG, "host connected");
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        // The session starts when the host subscribes to TX, i.e. when it can
        // hear us: service discovery before that doesn't eat the handshake timeout.
        if (ev->subscribe.attr_handle == tx_handle && ev->subscribe.cur_notify && session == SMART_NO_SESSION) {
            session = smart_esp_link_open(&LINK);
            if (session == SMART_NO_SESSION)
                ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "host disconnected");
        if (session != SMART_NO_SESSION)
            smart_esp_link_gone(session);
        session = SMART_NO_SESSION;
        conn = BLE_HS_CONN_HANDLE_NONE;
        advertise();
        break;
    case BLE_GAP_EVENT_MTU:
        mtu = ev->mtu.value;
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    default:
        break;
    }
    return 0;
}

// Advertise if wanted and possible (one host at a time).
static void advertise(void)
{
    if (!synced || !want_adv || conn != BLE_HS_CONN_HANDLE_NONE || ble_gap_adv_active())
        return;

    struct ble_hs_adv_fields adv = { 0 };
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.name = (const uint8_t *)name;
    adv.name_len = (uint8_t)strlen(name);
    adv.name_is_complete = 1;
    // the 128-bit service UUID doesn't fit next to the name: scan response
    struct ble_hs_adv_fields rsp = { 0 };
    rsp.uuids128 = &SVC_UUID;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;

    struct ble_gap_adv_params params = { 0 };
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    if (ble_gap_adv_set_fields(&adv) || ble_gap_adv_rsp_set_fields(&rsp) ||
        ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL))
        ESP_LOGW(TAG, "can't start advertising");
    else
        ESP_LOGI(TAG, "advertising as \"%s\"", name);
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &own_addr_type);
    synced = true;
    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset, reason %d", reason);
    synced = false;
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ---------- public ----------
void smart_ble_init(const char *device_name)
{
    strncpy(name, device_name, sizeof(name) - 1);
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(SERVICES));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(SERVICES));
    ble_svc_gap_device_name_set(name);
    nimble_port_freertos_init(host_task);
}

void smart_ble_start(void)
{
    want_adv = true;
    advertise();
}

void smart_ble_stop(void)
{
    want_adv = false;
    if (ble_gap_adv_active())
        ble_gap_adv_stop();
    if (conn != BLE_HS_CONN_HANDLE_NONE)
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    ESP_LOGI(TAG, "off");
}
