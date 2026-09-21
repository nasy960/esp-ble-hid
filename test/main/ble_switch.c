#include "ble_switch.h"
#include "input_router.h"
#include "ble_report_map.h"
#include "status_led.h"
#include "mouse_pending.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define PEERS_MAX 8
#define CONNECTIONS 2
#define NO_HANDLE 0xffff
static const char *TAG = "ble_switch";
static const char *DEVICE_NAME = "ESP USB Switch";
static QueueHandle_t input_queue;
static struct ble_npl_callout poll_timer;
static struct ble_npl_event input_wakeup;
static input_router_t router;
static uint8_t own_addr_type;
static int pairing_slot = -1;
static bool reset_pending;
static int64_t pairing_deadline;
static nvs_handle_t peer_nvs;
static portMUX_TYPE overflow_lock = portMUX_INITIALIZER_UNLOCKED;
static bool overflow;
// Connection state is separate from the input queue: idle keyboards may never
// send a report, and queue overflow must not lose USB presence information.
static uint32_t usb_sources;

typedef struct {
    uint8_t valid, slot, type, address[6];
} peer_t;
static peer_t peers[PEERS_MAX];
typedef struct {
    uint16_t handle;
    int slot;
    bool encrypted, bonded, keyboard_sub, mouse_sub, suspended;
    bool key_dirty, mouse_dirty, release_pending, switch_release;
    bool pairing_candidate;
    bool params_requested;
    unsigned params_attempts;
    uint32_t notify_failures, mouse_overflows;
    int64_t params_retry_at;
    mouse_pending_t mouse_pending;
    uint8_t keyboard[8], buttons, leds;
    int64_t connected_at, release_started;
} connection_t;
static connection_t connections[CONNECTIONS];
typedef struct {
    enum { INPUT, REMOVE, COMMAND } type;
    unsigned source;
    hid_input_t input;
    char command[24];
} input_event_t;
static uint16_t keyboard_handle, mouse_handle;
static const uint8_t key_reference[] = {1, 1}, mouse_reference[] = {2, 1}, led_reference[] = {1, 2};

static connection_t *by_handle(uint16_t handle)
{
    for (unsigned i = 0; i < CONNECTIONS; ++i)
        if (connections[i].handle == handle) return &connections[i];
    return NULL;
}
static connection_t *by_slot(unsigned slot)
{
    for (unsigned i = 0; i < CONNECTIONS; ++i)
        if (connections[i].handle != NO_HANDLE && connections[i].slot == (int)slot) return &connections[i];
    return NULL;
}
static bool subscribed(const connection_t *c)
{
    return c && c->encrypted && c->bonded && c->keyboard_sub && c->mouse_sub && !c->suspended;
}
static bool ready(unsigned slot)
{
    connection_t *c = by_slot(slot);
    return subscribed(c) && !c->release_pending;
}
static int append(struct os_mbuf *om, const void *data, size_t len)
{
    return os_mbuf_append(om, data, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}
enum { ATTR_INFO, ATTR_MAP, ATTR_CONTROL, ATTR_KEYS, ATTR_MOUSE, ATTR_LEDS, ATTR_BATTERY, ATTR_MANUFACTURER };
static void neutralize(connection_t *c)
{
    mouse_pending_clear(&c->mouse_pending);
    memset(c->keyboard, 0, sizeof(c->keyboard)); c->buttons = 0;
    c->key_dirty = c->mouse_dirty = c->release_pending = true;
}
static int access_report(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    connection_t *c = by_handle(conn);
    uintptr_t which = (uintptr_t)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC) return append(ctxt->om, arg, 2);
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t value;
        if (!c || OS_MBUF_PKTLEN(ctxt->om) != 1 || os_mbuf_copydata(ctxt->om, 0, 1, &value))
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (which == ATTR_LEDS) c->leds = value;
        else if (which == ATTR_CONTROL) {
            if (value > 1) return BLE_ATT_ERR_UNLIKELY;
            c->suspended = value == 0;
            neutralize(c);
            input_router_block_held(&router);
        } else return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
        return 0;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    static const uint8_t info[] = {0x11, 0x01, 0, 2}, battery = 100;
    static const uint8_t zero_keys[8];
    uint8_t mouse[7] = {0};
    switch (which) {
    case ATTR_INFO: return append(ctxt->om, info, sizeof(info));
    case ATTR_MAP: return append(ctxt->om, report_map, sizeof(report_map));
    case ATTR_KEYS: return append(ctxt->om, c ? c->keyboard : zero_keys, 8);
    case ATTR_MOUSE:
        mouse[0] = c ? c->buttons : 0;
        return append(ctxt->om, mouse, sizeof(mouse));
    case ATTR_LEDS: mouse[0] = c ? c->leds : 0; return append(ctxt->om, mouse, 1);
    case ATTR_BATTERY: return append(ctxt->om, &battery, 1);
    case ATTR_MANUFACTURER: return append(ctxt->om, "USB BLE Switch", 14);
    default: return BLE_ATT_ERR_READ_NOT_PERMITTED;
    }
}
#define UUID16(n) BLE_UUID16_DECLARE(n)
#define REF_DESCRIPTOR(ref) (struct ble_gatt_dsc_def[]) { \
    {.uuid=UUID16(0x2908), .att_flags=BLE_ATT_F_READ, .access_cb=access_report, .arg=(void *)(ref)}, {0} }
static const struct ble_gatt_svc_def services[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = UUID16(0x1812),
      .characteristics = (struct ble_gatt_chr_def[]) {
        {.uuid=UUID16(0x2a4a), .access_cb=access_report, .arg=(void *)ATTR_INFO, .flags=BLE_GATT_CHR_F_READ},
        {.uuid=UUID16(0x2a4b), .access_cb=access_report, .arg=(void *)ATTR_MAP, .flags=BLE_GATT_CHR_F_READ},
        {.uuid=UUID16(0x2a4c), .access_cb=access_report, .arg=(void *)ATTR_CONTROL,
         .flags=BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC},
        {.uuid=UUID16(0x2a4d), .access_cb=access_report, .arg=(void *)ATTR_KEYS,
         .flags=BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_NOTIFY,
         .val_handle=&keyboard_handle, .descriptors=REF_DESCRIPTOR(key_reference)},
        {.uuid=UUID16(0x2a4d), .access_cb=access_report, .arg=(void *)ATTR_MOUSE,
         .flags=BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_NOTIFY,
         .val_handle=&mouse_handle, .descriptors=REF_DESCRIPTOR(mouse_reference)},
        {.uuid=UUID16(0x2a4d), .access_cb=access_report, .arg=(void *)ATTR_LEDS,
         .flags=BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_WRITE |
                BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC,
         .descriptors=REF_DESCRIPTOR(led_reference)}, {0}
      }},
    {.type=BLE_GATT_SVC_TYPE_PRIMARY, .uuid=UUID16(0x180f),
     .characteristics=(struct ble_gatt_chr_def[]) {
        {.uuid=UUID16(0x2a19), .access_cb=access_report, .arg=(void *)ATTR_BATTERY, .flags=BLE_GATT_CHR_F_READ}, {0}
     }},
    {.type=BLE_GATT_SVC_TYPE_PRIMARY, .uuid=UUID16(0x180a),
     .characteristics=(struct ble_gatt_chr_def[]) {
        {.uuid=UUID16(0x2a29), .access_cb=access_report, .arg=(void *)ATTR_MANUFACTURER, .flags=BLE_GATT_CHR_F_READ}, {0}
     }}, {0}
};

static int find_peer(const ble_addr_t *addr)
{
    for (unsigned i = 0; i < PEERS_MAX; ++i)
        if (peers[i].valid && peers[i].type == (addr->type & 1) &&
            !memcmp(peers[i].address, addr->val, 6)) return i;
    return -1;
}
static bool save_peer(const ble_addr_t *addr, int slot)
{
    int i = find_peer(addr);
    if (i < 0) for (unsigned j = 0; j < PEERS_MAX; ++j) if (!peers[j].valid) { i = j; break; }
    if (i < 0) return false;
    peer_t previous = peers[i];
    peers[i] = (peer_t){.valid=1, .slot=slot, .type=addr->type & 1};
    memcpy(peers[i].address, addr->val, 6);
    esp_err_t err = nvs_set_blob(peer_nvs, "peers_v1", peers, sizeof(peers));
    if (err == ESP_OK) err = nvs_commit(peer_nvs);
    if (err != ESP_OK) {
        peers[i] = previous;
        ESP_LOGE(TAG, "Cannot save PC identity: %s", esp_err_to_name(err));
    }
    return err == ESP_OK;
}
static bool enrolling(void) { return pairing_slot >= 0 && esp_timer_get_time() < pairing_deadline; }
static int gap_event(struct ble_gap_event *event, void *arg);
static void advertise(void)
{
    if (reset_pending) return;
    unsigned used = 0;
    for (unsigned i = 0; i < CONNECTIONS; ++i) used += connections[i].handle != NO_HANDLE;
    if (used == CONNECTIONS || ble_gap_adv_active() || !ble_hs_synced()) return;
    const ble_uuid16_t hid_uuid = BLE_UUID16_INIT(0x1812);
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids16 = &hid_uuid; fields.num_uuids16 = 1; fields.uuids16_is_complete = 1;
    fields.appearance = 0x03c1; fields.appearance_is_present = 1;
    fields.name = (uint8_t *)DEVICE_NAME; fields.name_len = strlen(DEVICE_NAME); fields.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    struct ble_gap_adv_params params = { .conn_mode=BLE_GAP_CONN_MODE_UND, .disc_mode=BLE_GAP_DISC_MODE_GEN,
                                        .itvl_min=160, .itvl_max=240 };
    if (!rc) rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc && rc != BLE_HS_EALREADY) ESP_LOGW(TAG, "Advertising failed rc=%d", rc);
}
static void disconnect(connection_t *c)
{
    if (c) ble_gap_terminate(c->handle, BLE_ERR_REM_USER_CONN_TERM);
}
static void retire(connection_t *c)
{
    if (!c) return;
    neutralize(c);
    c->switch_release = true;
    c->release_started = esp_timer_get_time();
    if (!subscribed(c)) disconnect(c);
}
static void log_connection(connection_t *c)
{
    struct ble_gap_conn_desc desc;
    if (c && !ble_gap_conn_find(c->handle, &desc)) {
        unsigned us = desc.conn_itvl * 1250;
        ESP_LOGI(TAG, "PC %c BLE interval=%u.%02u ms latency=%u timeout=%u ms",
                 'A'+c->slot, us/1000, (us%1000)/10,
                 desc.conn_latency, desc.supervision_timeout*10);
    }
}
static void request_fast_connection(connection_t *c)
{
    if (!c->encrypted || c->params_requested || c->params_attempts >= 3 ||
        esp_timer_get_time() < c->params_retry_at) return;
    const struct ble_gap_upd_params params = {
        .itvl_min=6, .itvl_max=12, .latency=0, .supervision_timeout=400,
    }; // 7.5-15 ms; the PC makes the final choice.
    int rc = ble_gap_update_params(c->handle, &params);
    ++c->params_attempts;
    c->params_retry_at = esp_timer_get_time()+2000000;
    if (!rc) c->params_requested = true;
    ESP_LOGI(TAG, "PC %c requested 7.5-15 ms BLE interval, latency=0: rc=%d", 'A'+c->slot, rc);
}
static int gap_event(struct ble_gap_event *event, void *arg)
{
    connection_t *c;
    struct ble_gap_conn_desc desc;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status) return 0; // Periodic task restarts advertising.
        if (ble_gap_conn_find(event->connect.conn_handle, &desc)) return 0;
        c = NULL;
        for (unsigned i = 0; i < CONNECTIONS; ++i) if (connections[i].handle == NO_HANDLE) { c = &connections[i]; break; }
        if (!c) { ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM); return 0; }
        memset(c, 0, sizeof(*c)); c->handle = event->connect.conn_handle; c->slot = -1;
        c->connected_at = esp_timer_get_time();
        c->params_retry_at = c->connected_at+1000000;
        if (reset_pending) { disconnect(c); return 0; }
        int peer = find_peer(&desc.peer_id_addr);
        int slot = peer >= 0 ? peers[peer].slot : enrolling() ? pairing_slot : -1;
        if (slot < 0 || by_slot(slot)) {
            ESP_LOGW(TAG, "Unregistered/duplicate PC rejected; use 'pair a' or 'pair b' on COM");
            disconnect(c); return 0;
        }
        c->slot = slot;
        c->pairing_candidate = peer < 0 && enrolling() && pairing_slot == slot;
        neutralize(c);
        ESP_LOGI(TAG, "PC %c connected, handle=%u; waiting for encryption and HID subscriptions", 'A'+slot, c->handle);
        log_connection(c);
        int rc = ble_gap_security_initiate(c->handle);
        if (rc && rc != BLE_HS_EALREADY) { ESP_LOGW(TAG, "Security start rc=%d", rc); disconnect(c); }
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
        c = by_handle(event->enc_change.conn_handle);
        if (!c || ble_gap_conn_find(c->handle, &desc)) return 0;
        if (reset_pending) { disconnect(c); return 0; }
        if (event->enc_change.status || !desc.sec_state.encrypted || !desc.sec_state.bonded) {
            ESP_LOGW(TAG, "PC encryption/bond failed; enroll that OS again if its keys changed");
            disconnect(c); return 0;
        }
        peer = find_peer(&desc.peer_id_addr);
        if (peer < 0 && (!enrolling() || pairing_slot != c->slot)) { disconnect(c); return 0; }
        if (peer >= 0 && peers[peer].slot != c->slot) { disconnect(c); return 0; }
        if (!save_peer(&desc.peer_id_addr, c->slot)) { disconnect(c); return 0; }
        c->encrypted = c->bonded = true;
        neutralize(c); input_router_block_held(&router);
        ESP_LOGI(TAG, "PC %c bonded and encrypted (handle=%u)", 'A'+c->slot, c->handle);
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        c = by_handle(event->subscribe.conn_handle);
        if (!c) return 0;
        if (event->subscribe.attr_handle == keyboard_handle) c->keyboard_sub = event->subscribe.cur_notify;
        if (event->subscribe.attr_handle == mouse_handle) c->mouse_sub = event->subscribe.cur_notify;
        neutralize(c); input_router_block_held(&router);
        ESP_LOGI(TAG, "PC %c subscriptions keyboard=%d mouse=%d", 'A'+c->slot, c->keyboard_sub, c->mouse_sub);
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        c = by_handle(event->disconnect.conn.conn_handle);
        if (c) {
            ESP_LOGI(TAG, "PC %c disconnected reason=%d", c->slot >= 0 ? 'A'+c->slot : '?', event->disconnect.reason);
            memset(c, 0, sizeof(*c)); c->handle = NO_HANDLE; c->slot = -1;
        }
        input_router_block_held(&router);
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        c = by_handle(event->repeat_pairing.conn_handle);
        if (!c || !enrolling() || c->slot != pairing_slot || ble_gap_conn_find(c->handle, &desc))
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        ble_store_util_delete_peer(&desc.peer_id_addr);
        c->pairing_candidate = true;
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    case BLE_GAP_EVENT_ADV_COMPLETE: return 0;
    case BLE_GAP_EVENT_CONN_UPDATE:
        c = by_handle(event->conn_update.conn_handle);
        if (event->conn_update.status)
            ESP_LOGW(TAG, "BLE connection parameter update status=%d", event->conn_update.status);
        log_connection(c);
        return 0;
    default: return 0;
    }
}
static bool notify(connection_t *c, uint16_t attr, const uint8_t *data, size_t length)
{
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, length);
    if (!om) { ++c->notify_failures; return false; }
    // Target an individual connection. Never broadcast HID notifications.
    int rc = ble_gatts_notify_custom(c->handle, attr, om);
    if (rc) ++c->notify_failures;
    return rc == 0;
}
static void switch_to(unsigned target);
static void flush_state(connection_t *c)
{
    if (!subscribed(c)) return;
    if (c->key_dirty && notify(c, keyboard_handle, c->keyboard, 8)) c->key_dirty = false;
    uint8_t mouse[7] = {c->buttons};
    if (c->release_pending) {
        if (c->mouse_dirty && notify(c, mouse_handle, mouse, 7)) c->mouse_dirty = false;
    } else {
        // Limit each drain so controller events and the other PC remain responsive.
        for (unsigned n=0; n<4 && mouse_pending_peek(&c->mouse_pending, mouse); ++n) {
            if (!notify(c, mouse_handle, mouse, 7)) break;
            mouse_pending_commit(&c->mouse_pending, mouse);
        }
        c->mouse_dirty = c->mouse_pending.count != 0;
    }
    if (c->release_pending && !c->key_dirty && !c->mouse_dirty) {
        c->release_pending = false;
        c->switch_release = false;
        ESP_LOGI(TAG, "PC %c ready; selected=%c", 'A'+c->slot, 'A'+router.target);
        if (c->pairing_candidate && enrolling() && pairing_slot == c->slot) {
            pairing_slot = -1;
            ESP_LOGI(TAG, "PC %c registration complete; pairing closed", 'A'+c->slot);
            switch_to(c->slot);
        }
        c->pairing_candidate = false;
    }
}
static bool previous_release_pending(void)
{
    connection_t *c = by_slot(router.target ^ 1);
    return c && c->switch_release && c->release_pending;
}
static void switch_to(unsigned target)
{
    if (!ready(target)) { ESP_LOGW(TAG, "PC %c is not ready; keeping current target", 'A'+target); return; }
    if (target == router.target) return;
    connection_t *old = by_slot(router.target), *next = by_slot(target);
    retire(old);
    neutralize(next);
    input_router_select(&router, target);
    ESP_LOGI(TAG, "Selected PC %c; releasing old keys/buttons", 'A'+target);
}
static void begin_pairing(unsigned slot)
{
    if (reset_pending) return;
    pairing_slot = slot;
    pairing_deadline = esp_timer_get_time() + 120000000;
    input_router_block_held(&router);
    connection_t *current = by_slot(slot);
    if (current) {
        // Free this slot for enrollment without removing any stored identities.
        current->pairing_candidate = false;
        neutralize(current);
        flush_state(current);
        disconnect(current);
    }
    ESP_LOGI(TAG, "Pairing PC %c for 120 seconds. Select '%s' in that OS's Bluetooth settings.", 'A'+slot, DEVICE_NAME);
}
static void cancel_pairing(void)
{
    pairing_slot = -1;
    ESP_LOGI(TAG, "Pairing closed");
}
static void reset_registrations(void)
{
    pairing_slot = -1;
    reset_pending = true;
    ble_gap_adv_stop();
    input_router_block_held(&router);
    for (unsigned i = 0; i < CONNECTIONS; ++i) {
        connection_t *c = &connections[i];
        if (c->handle == NO_HANDLE) continue;
        c->pairing_candidate = false;
        neutralize(c);
        flush_state(c);
        disconnect(c);
    }
    ESP_LOGI(TAG, "Clearing all A/B registrations after BLE disconnects");
}
static void finish_reset(void)
{
    if (!reset_pending) return;
    // Wait for disconnect callbacks so an in-flight pairing cannot restore a bond.
    for (unsigned i = 0; i < CONNECTIONS; ++i)
        if (connections[i].handle != NO_HANDLE) return;
    int rc = ble_store_clear();
    if (rc) {
        ESP_LOGE(TAG, "Clearing Bluetooth bonds failed rc=%d; retry reset shortcut", rc);
        reset_pending = false;
        return;
    }
    esp_err_t err = nvs_erase_key(peer_nvs, "peers_v1");
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(peer_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Clearing saved identities failed: %s; retry reset shortcut", esp_err_to_name(err));
        reset_pending = false;
        return;
    }
    memset(peers, 0, sizeof(peers));
    input_router_select(&router, 0);
    reset_pending = false;
    ESP_LOGI(TAG, "All A/B registrations cleared; remove '%s' from each OS before pairing again", DEVICE_NAME);
}
static void run_command(const char *line)
{
    if (!strcmp(line, "pair a") || !strcmp(line, "pair b")) {
        begin_pairing(line[5] == 'a' ? 0 : 1);
    } else if (!strcmp(line, "pair off")) {
        cancel_pairing();
    } else if (!strcmp(line, "forget a") || !strcmp(line, "forget b")) {
        unsigned slot = line[7] == 'a' ? 0 : 1;
        disconnect(by_slot(slot));
        for (unsigned i = 0; i < PEERS_MAX; ++i) if (peers[i].valid && peers[i].slot == slot) {
            ble_addr_t address = {.type=peers[i].type};
            memcpy(address.val, peers[i].address, 6);
            int rc = ble_store_util_delete_peer(&address);
            if (rc && rc != BLE_HS_ENOENT) {
                ESP_LOGW(TAG, "Could not remove bond %u rc=%d", i, rc);
                continue;
            }
            peers[i].valid = 0;
        }
        esp_err_t err = nvs_set_blob(peer_nvs, "peers_v1", peers, sizeof(peers));
        if (err == ESP_OK) err = nvs_commit(peer_nvs);
        if (err != ESP_OK) ESP_LOGE(TAG, "Saving removed identities: %s", esp_err_to_name(err));
        ESP_LOGI(TAG, "PC %c registration cleared; also forget '%s' in that PC's OS settings", 'A'+slot, DEVICE_NAME);
    } else if (!strcmp(line, "a") || !strcmp(line, "b")) {
        switch_to(line[0] == 'a' ? 0 : 1);
    } else if (!strcmp(line, "status")) {
        ESP_LOGI(TAG, "Selected=%c A-ready=%d B-ready=%d pairing=%c", 'A'+router.target,
                 ready(0), ready(1), enrolling() ? 'A'+pairing_slot : '-');
        for (unsigned i=0; i<CONNECTIONS; ++i) if (connections[i].handle != NO_HANDLE) {
            connection_t *c=&connections[i];
            log_connection(c);
            ESP_LOGI(TAG, "PC %c pending_mouse=%u notify_failures=%lu mouse_overflows=%lu",
                     c->slot >= 0 ? 'A'+c->slot : '?', c->mouse_pending.count,
                     (unsigned long)c->notify_failures, (unsigned long)c->mouse_overflows);
        }
        for (unsigned i = 0; i < PEERS_MAX; ++i) if (peers[i].valid)
            ESP_LOGI(TAG, "Saved identity %u -> PC %c", i, 'A'+peers[i].slot);
    } else {
        ESP_LOGI(TAG, "Commands: pair a | pair b | pair off | a | b | status | forget a | forget b | help");
        ESP_LOGI(TAG, "Mouse button %d or Ctrl+Alt+F12 switches PCs. Register each OS to its PC slot.", CONFIG_INPUT_SWITCH_BUTTON);
        ESP_LOGI(TAG, "Ctrl+Alt+Shift+Left/Right enrolls A/B; Ctrl+Alt+Shift+Down cancels pairing.");
        ESP_LOGI(TAG, "Hold Ctrl+Alt+Shift+Backspace for 3 seconds to erase all A/B registrations.");
    }
}
static void process_input(const input_event_t *event)
{
    unsigned old_target = router.target;
    router_output_t output;
    input_router_apply(&router, event->source, &event->input, event->type == REMOVE,
                       ready(router.target ^ 1) && !previous_release_pending(), &output);
    if (reset_pending) { input_router_block_held(&router); return; }
    if (output.pairing_action == ROUTER_PAIR_A) begin_pairing(0);
    else if (output.pairing_action == ROUTER_PAIR_B) begin_pairing(1);
    else if (output.pairing_action == ROUTER_PAIR_CANCEL) cancel_pairing();
    if (output.switched) {
        connection_t *old = by_slot(old_target), *next = by_slot(router.target);
        retire(old);
        if (next) neutralize(next);
        ESP_LOGI(TAG, "Selected PC %c (mouse button / Ctrl+Alt+F12)", 'A'+router.target);
        return;
    }
    if (output.switch_requested) ESP_LOGW(TAG, "Other PC not ready; switch ignored");
    if (!ready(router.target) || previous_release_pending()) {
        input_router_block_held(&router); return;
    }
    connection_t *c = by_slot(router.target);
    if (memcmp(c->keyboard, output.keyboard, 8)) {
        memcpy(c->keyboard, output.keyboard, 8); c->key_dirty = true;
    }
    bool buttons_changed = c->buttons != output.mouse[0];
    c->buttons = output.mouse[0];
    bool moving = false;
    for (unsigned i = 1; i < 7; ++i) moving |= output.mouse[i] != 0;
    if ((moving || buttons_changed) && !mouse_pending_push(&c->mouse_pending, output.mouse)) {
        ++c->mouse_overflows;
        neutralize(c);
        input_router_block_held(&router);
    }
    flush_state(c);
}
static void poll(struct ble_npl_event *ev)
{
    if (pairing_slot >= 0 && !enrolling()) { pairing_slot = -1; ESP_LOGI(TAG, "Pairing window expired"); }
    portENTER_CRITICAL(&overflow_lock); bool lost = overflow; overflow = false; portEXIT_CRITICAL(&overflow_lock);
    if (lost) {
        input_event_t discard;
        while (xQueueReceive(input_queue, &discard, 0)) {}
        unsigned target = router.target;
        input_router_init(&router, CONFIG_INPUT_SWITCH_BUTTON); router.target = target;
        for (unsigned i = 0; i < CONNECTIONS; ++i) if (connections[i].handle != NO_HANDLE) neutralize(&connections[i]);
        ESP_LOGW(TAG, "Input queue overflow: released all keys/buttons");
    }
    for (unsigned i = 0; i < CONNECTIONS; ++i) {
        connection_t *c = &connections[i];
        if (c->handle == NO_HANDLE) continue;
        request_fast_connection(c);
        if (!c->encrypted && esp_timer_get_time() - c->connected_at > 60000000) disconnect(c);
        flush_state(c);
        if (c->switch_release && c->release_pending && esp_timer_get_time() - c->release_started > 3000000) {
            ESP_LOGW(TAG, "PC %c release timed out; disconnecting before routing more input", 'A'+c->slot);
            disconnect(c);
        }
    }
    input_event_t event;
    for (unsigned n = 0; n < 64 && xQueueReceive(input_queue, &event, 0); ++n) {
        if (event.type == COMMAND) run_command(event.command);
        else process_input(&event);
    }
    if (!uxQueueMessagesWaiting(input_queue) && !reset_pending &&
        input_router_reset_due(&router, esp_timer_get_time()/1000)) reset_registrations();
    finish_reset();
    if (uxQueueMessagesWaiting(input_queue)) ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &input_wakeup);
    advertise();
    portENTER_CRITICAL(&overflow_lock);
    bool no_usb_devices = usb_sources == 0;
    portEXIT_CRITICAL(&overflow_lock);
    status_led_update(router.target, ready(router.target) && !previous_release_pending(),
                      enrolling() ? pairing_slot : -1, no_usb_devices);
    ble_npl_callout_reset(&poll_timer, ble_npl_time_ms_to_ticks32(10));
}
static void on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset reason=%d", reason);
    for (unsigned i = 0; i < CONNECTIONS; ++i) { connections[i].handle = NO_HANDLE; connections[i].slot = -1; }
    input_router_block_held(&router);
}
static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (!rc) rc = ble_hs_id_infer_auto(0, &own_addr_type);
    assert(rc == 0);
    advertise();
    ble_npl_callout_reset(&poll_timer, ble_npl_time_ms_to_ticks32(10));
}
static void host_task(void *arg) { nimble_port_run(); nimble_port_freertos_deinit(); }
void ble_store_config_init(void);
esp_err_t ble_switch_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) return err; // Do not silently erase existing bonds or application NVS.
    err = nvs_open("input_switch", NVS_READWRITE, &peer_nvs);
    if (err != ESP_OK) return err;
    size_t size = sizeof(peers);
    err = nvs_get_blob(peer_nvs, "peers_v1", peers, &size);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
    if (err == ESP_OK && size != sizeof(peers)) return ESP_ERR_INVALID_SIZE;
    for (unsigned i = 0; i < PEERS_MAX; ++i) if (peers[i].valid && peers[i].slot > 1) return ESP_ERR_INVALID_STATE;
    input_queue = xQueueCreate(128, sizeof(input_event_t));
    if (!input_queue) return ESP_ERR_NO_MEM;
    input_router_init(&router, CONFIG_INPUT_SWITCH_BUTTON);
    for (unsigned i = 0; i < CONNECTIONS; ++i) { connections[i].handle = NO_HANDLE; connections[i].slot = -1; }
    err = nimble_port_init();
    if (err != ESP_OK) return err;
    ble_hs_cfg.sync_cb = on_sync; ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1; ble_hs_cfg.sm_sc = 1; ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    // No round-robin bond deletion: fail pairing when full rather than lose another OS.
    ble_svc_gap_init(); ble_svc_gatt_init();
    int rc = ble_svc_gap_device_name_set(DEVICE_NAME);
    if (!rc) rc = ble_svc_gap_device_appearance_set(0x03c1);
    if (!rc) rc = ble_gatts_count_cfg(services);
    if (!rc) rc = ble_gatts_add_svcs(services);
    if (rc) return ESP_FAIL;
    ble_store_config_init();
    ble_npl_event_init(&input_wakeup, poll, NULL);
    ble_npl_callout_init(&poll_timer, nimble_port_get_dflt_eventq(), poll, NULL);
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "BLE HID switch started. COM commands: pair a / pair b / status / help");
    return ESP_OK;
}
static void enqueue(const input_event_t *event)
{
    if (xQueueSend(input_queue, event, 0) != pdTRUE) {
        portENTER_CRITICAL(&overflow_lock); overflow = true; portEXIT_CRITICAL(&overflow_lock);
    }
    // The 10-ms callout is a retry/maintenance fallback, not an input polling delay.
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &input_wakeup);
}
void ble_switch_input(unsigned source, const hid_input_t *input)
{
    input_event_t event = {.type=INPUT, .source=source, .input=*input}; enqueue(&event);
}
void ble_switch_usb_connected(unsigned source)
{
    if (source >= ROUTER_SOURCES) return;
    portENTER_CRITICAL(&overflow_lock); usb_sources |= 1u << source; portEXIT_CRITICAL(&overflow_lock);
}
void ble_switch_remove(unsigned source)
{
    if (source >= ROUTER_SOURCES) return;
    portENTER_CRITICAL(&overflow_lock); usb_sources &= ~(1u << source); portEXIT_CRITICAL(&overflow_lock);
    ble_switch_release_input(source);
}
void ble_switch_release_input(unsigned source)
{
    if (source >= ROUTER_SOURCES) return;
    input_event_t event = {.type=REMOVE, .source=source}; enqueue(&event);
}
void ble_switch_command(const char *command)
{
    input_event_t event = {.type=COMMAND};
    snprintf(event.command, sizeof(event.command), "%s", command); enqueue(&event);
}
