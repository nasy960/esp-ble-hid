#include "ble_switch.h"
#include "input_router.h"
#include "ble_report_map.h"
#include "status_led.h"
#include "os_profile.h"
#include "b_os_bonds.h"
#include "b_auth_gate.h"
#include "esp_system.h"
#include "psa/crypto.h"
#include "mouse_pending.h"
#include "keyboard_pending.h"
#include <assert.h>
#include <stdatomic.h>
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
#include "host/ble_l2cap.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define INPUT_MAX_DELAY_US 250000
#define RELEASE_TIMEOUT_US 3000000
#define DISCONNECT_RETRY_US 1000000
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
static atomic_uchar selected_leds;
uint8_t ble_switch_leds(void) { return atomic_load(&selected_leds); }
static int pairing_slot = -1;
static bool reset_pending;
static int profile_pending = -1;
static int64_t profile_deadline;
static uint32_t name_probe_serial, params_serial;
static int64_t pairing_deadline;
static nvs_handle_t peer_nvs;
static portMUX_TYPE overflow_lock = portMUX_INITIALIZER_UNLOCKED;
static bool overflow;
// Connection state is separate from the input queue: idle keyboards may never
// send a report, and queue overflow must not lose USB presence information.
static uint32_t usb_sources, usb_input_reports;
static uint8_t source_keys[ROUTER_SOURCES][HID_KEYS_BYTES];
static uint8_t source_buttons[ROUTER_SOURCES];

typedef struct {
    uint8_t valid, slot, type, address[6];
} peer_t;
static peer_t peers[PEERS_MAX];
typedef struct {
    uint16_t handle;
    int slot;
    bool encrypted, bonded, keyboard_sub, mouse_sub, suspended;
    bool key_dirty, mouse_dirty, release_pending, switch_release;
    // Quarantine immediately; completion of GAP disconnect is asynchronous.
    bool disconnecting, terminate_requested;
    int64_t disconnect_retry_at;
    uint32_t disconnect_failures;
    bool pairing_candidate;
    bool name_pending, bond_trial;
    uint32_t name_token;
    int64_t name_deadline;
    char peer_name[65];
    bool params_requested, params_l2cap;
    uint32_t params_token;
    unsigned params_attempts;
    uint32_t notify_failures, mouse_overflows, key_overflows, stale_drops;
    int64_t backlog_started;
    int64_t params_retry_at;
    mouse_pending_t mouse_pending;
    keyboard_pending_t key_pending;
    uint8_t keyboard[8], buttons, leds;
    int64_t connected_at, release_started;
} connection_t;
static connection_t connections[CONNECTIONS];
typedef struct {
    enum { INPUT, REMOVE, COMMAND } type;
    unsigned source;
    int64_t queued_at;
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
    return c && !c->disconnecting && c->encrypted && c->bonded &&
           c->keyboard_sub && c->mouse_sub && !c->suspended;
}
static bool ready(unsigned slot)
{
    connection_t *c = by_slot(slot);
    return subscribed(c) && !c->release_pending;
}
static void block_selected_held(const connection_t *c)
{
    // A standby PC's power/subscription events must not alter active input.
    if (c && c->slot == (int)router.target) input_router_block_held(&router);
}
static int append(struct os_mbuf *om, const void *data, size_t len)
{
    return os_mbuf_append(om, data, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}
enum { ATTR_INFO, ATTR_MAP, ATTR_CONTROL, ATTR_KEYS, ATTR_MOUSE, ATTR_LEDS, ATTR_BATTERY, ATTR_MANUFACTURER };
static void neutralize(connection_t *c)
{
    mouse_pending_clear(&c->mouse_pending);
    keyboard_pending_clear(&c->key_pending);
    c->backlog_started = 0;
    if (!c->release_pending) c->release_started = 0;
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
            bool suspended = value == 0;
            if (c->suspended == suspended) return 0;
            c->suspended = suspended;
            neutralize(c);
            block_selected_held(c);
            ESP_LOGI(TAG, "PC %c HID %s; selected=%c", 'A'+c->slot,
                     suspended ? "suspended" : "resumed", 'A'+router.target);
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
    if (reset_pending || profile_pending >= 0) return;
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
    if (!c || c->handle == NO_HANDLE) return;
    if (!c->disconnecting) {
        b_auth_gate_cancel(c->handle);
        neutralize(c);
        c->disconnecting = true;
        c->switch_release = false;
        c->pairing_candidate = false;
        block_selected_held(c);
    }
    if (c->terminate_requested || esp_timer_get_time() < c->disconnect_retry_at) return;
    c->disconnect_retry_at = esp_timer_get_time() + DISCONNECT_RETRY_US;
    int rc = ble_gap_terminate(c->handle, BLE_ERR_REM_USER_CONN_TERM);
    if (rc == 0 || rc == BLE_HS_EALREADY) {
        c->terminate_requested = true;
    } else if (rc == BLE_HS_ENOTCONN) {
        // GAP confirms the handle is gone; do not wait for an event indefinitely.
        ESP_LOGI(TAG, "PC %c already disconnected; clearing stale handle", 'A'+c->slot);
        memset(c, 0, sizeof(*c)); c->handle = NO_HANDLE; c->slot = -1;
    } else {
        ++c->disconnect_failures;
        ESP_LOGW(TAG, "PC %c disconnect rc=%d; isolated, retrying in 1 second", 'A'+c->slot, rc);
    }
}
static void retire(connection_t *c)
{
    if (!c || c->disconnecting) return;
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
static void params_l2cap_done(uint16_t handle, int status, void *arg)
{
    connection_t *c=by_handle(handle);
    if (!c || c->disconnecting || c->params_token!=(uint32_t)(uintptr_t)arg) return;
    ESP_LOGI(TAG,"PC %c L2CAP interval request response=%d; actual interval follows in connection update",'A'+c->slot,status);
    if (status) {
        c->params_requested=false;
        c->params_retry_at=esp_timer_get_time()+5000000;
    }
}
static void request_fast_connection(connection_t *c)
{
    if (c->disconnecting || c->suspended || !c->encrypted || c->params_requested || c->params_attempts >= 3 ||
        esp_timer_get_time() < c->params_retry_at) return;
    struct ble_gap_conn_desc desc;
    if (!ble_gap_conn_find(c->handle,&desc) && desc.conn_itvl == 6 && !desc.conn_latency) {
        c->params_requested=true; // Already 7.5 ms; leave the working link alone.
        return;
    }
    int rc;
    if (c->params_l2cap) {
        struct ble_l2cap_sig_update_params params={
            .itvl_min=6,.itvl_max=6,.slave_latency=0,.timeout_multiplier=400,
        };
        c->params_token=++params_serial;
        rc=ble_l2cap_sig_update(c->handle,&params,params_l2cap_done,(void *)(uintptr_t)c->params_token);
    } else {
        const struct ble_gap_upd_params params={
            .itvl_min=6,.itvl_max=6,.latency=0,.supervision_timeout=400,
        };
        rc=ble_gap_update_params(c->handle,&params);
    }
    ++c->params_attempts;
    c->params_retry_at=esp_timer_get_time()+2000000;
    if (!rc) c->params_requested=true;
    ESP_LOGI(TAG,"PC %c requested 7.5 ms via %s: rc=%d",'A'+c->slot,c->params_l2cap ? "L2CAP" : "GAP/HCI",rc);
    if (!c->params_l2cap && rc==BLE_HS_HCI_ERR(BLE_ERR_INV_HCI_CMD_PARMS)) {
        c->params_l2cap=true;
        ESP_LOGW(TAG,"PC %c controller rejected interval command; next attempt asks central via L2CAP",'A'+c->slot);
    }
}
static void start_security(connection_t *c)
{
    struct ble_gap_conn_desc desc;
    if (c->slot == 1 && !c->bond_trial && !ble_gap_conn_find(c->handle, &desc) &&
        !b_os_bonds_select(&desc.peer_id_addr, c->peer_name, enrolling() && pairing_slot == 1)) {
        ESP_LOGW(TAG, "PC B OS name unavailable/unknown; leaving both OS bonds unchanged");
        disconnect(c); return;
    }
    if (b_auth_gate_resume(c->handle)) return;
    int rc = ble_gap_security_initiate(c->handle);
    if (rc && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "Security start rc=%d", rc);
        disconnect(c);
    }
}
static int name_read_done(uint16_t handle, const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr, void *arg)
{
    connection_t *c=by_handle(handle);
    if (!c || !c->name_pending || c->name_token!=(uint32_t)(uintptr_t)arg || c->disconnecting) return 0;
    c->name_pending=false;
    if (!error->status && attr && attr->om) {
        size_t length=OS_MBUF_PKTLEN(attr->om);
        if(length>sizeof(c->peer_name)-1) length=sizeof(c->peer_name)-1;
        if(!os_mbuf_copydata(attr->om,0,length,c->peer_name)) {
            while(length && c->peer_name[length-1]==0) --length;
            for(size_t i=0;i<length;++i)
                if((unsigned char)c->peer_name[i]<32 || c->peer_name[i]==127) c->peer_name[i]='?';
            c->peer_name[length]=0;
            ESP_LOGI(TAG,"PC B remote Device Name='%s' encrypted_at_reply=%d (%s)",c->peer_name,c->encrypted,c->bond_trial ? "diagnostic" : "enrollment selector");
        }
    } else ESP_LOGI(TAG,"PC B Device Name unavailable status=%d; continuing normal authentication",error->status);
    if (c->bond_trial) return 0; // Post-authentication name is diagnostic only.
    struct ble_gap_conn_desc desc;
    if (!ble_gap_conn_find(handle,&desc) && desc.sec_state.encrypted && desc.sec_state.bonded) {
        struct ble_gap_event done={.type=BLE_GAP_EVENT_ENC_CHANGE};
        done.enc_change.conn_handle=handle;
        b_auth_gate_cancel(handle);
        gap_event(&done,NULL);
    } else if (!c->encrypted) start_security(c);
    return 0;
}
static void probe_name(connection_t *c)
{
    static const ble_uuid16_t name_uuid=BLE_UUID16_INIT(0x2a00);
    c->name_pending=true;c->name_token=++name_probe_serial;
    c->name_deadline=esp_timer_get_time()+2000000;
    struct ble_gap_conn_desc desc;
    if(!ble_gap_conn_find(c->handle,&desc)) {
        const ble_addr_t *a=&desc.peer_ota_addr;
        ESP_LOGI(TAG,"PC B name probe: connection address type=%u %02X:%02X:%02X:%02X:%02X:%02X",
                 a->type,a->val[5],a->val[4],a->val[3],a->val[2],a->val[1],a->val[0]);
    }
    int rc=ble_gattc_read_by_uuid(c->handle,1,0xffff,&name_uuid.u,name_read_done,(void *)(uintptr_t)c->name_token);
    if(rc) {
        c->name_pending=false;
        ESP_LOGI(TAG,"PC B Device Name request rc=%d; continuing normal authentication",rc);
        start_security(c);
    }
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
        if (reset_pending || profile_pending >= 0) { disconnect(c); return 0; }
        int peer = find_peer(&desc.peer_id_addr);
        int slot = peer >= 0 ? peers[peer].slot : enrolling() ? pairing_slot : -1;
        if (slot < 0 || by_slot(slot)) {
            ESP_LOGW(TAG, "Unregistered/duplicate PC rejected; use 'pair a' or 'pair b' on COM");
            disconnect(c); return 0;
        }
        c->slot = slot;
        c->pairing_candidate = enrolling() && pairing_slot == slot;
        neutralize(c);
        ESP_LOGI(TAG, "PC %c connected, handle=%u; waiting for encryption and HID subscriptions", 'A'+slot, c->handle);
        log_connection(c);
        if (slot == 1) {
            b_os_bonds_begin(&desc.peer_id_addr);
            if (b_os_bonds_managed(&desc.peer_id_addr) && !(enrolling() && pairing_slot==1)) {
                c->bond_trial=true;
                if (!b_os_bonds_prepare(&desc.peer_id_addr)) { disconnect(c); return 0; }
                start_security(c);
            } else {
                if (b_os_bonds_managed(&desc.peer_id_addr)) b_auth_gate_begin(c->handle);
                probe_name(c);
            }
        }
        else start_security(c);
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
        c = by_handle(event->enc_change.conn_handle);
        if (!c || c->disconnecting || ble_gap_conn_find(c->handle, &desc)) return 0;
        if (reset_pending || profile_pending >= 0) { disconnect(c); return 0; }
        if (c->slot == 1 && !c->bond_trial && b_os_bonds_managed(&desc.peer_id_addr) && c->name_pending) {
            ESP_LOGW(TAG,"PC B authentication event before name: status=%d encrypted=%d bonded=%d; waiting for name",event->enc_change.status,desc.sec_state.encrypted,desc.sec_state.bonded);
            return 0;
        }
        if (c->slot == 1 && !c->bond_trial && b_os_bonds_managed(&desc.peer_id_addr) &&
            (!b_os_bonds_select(&desc.peer_id_addr,c->peer_name,enrolling() && pairing_slot==1))) {
            ESP_LOGW(TAG,"PC B OS identity unavailable; refusing unknown credentials");
            disconnect(c); return 0;
        }
        if (event->enc_change.status || !desc.sec_state.encrypted || !desc.sec_state.bonded) {
            if (c->bond_trial) b_os_bonds_failed(event->enc_change.status);
            ESP_LOGW(TAG, "PC encryption/bond failed status=%d; saved keys retained",event->enc_change.status);
            disconnect(c); return 0;
        }
        if (c->slot==1 && !b_os_bonds_ready(&desc.peer_id_addr)) {
            ESP_LOGW(TAG,"PC B OS bond not saved; use pair b and pair this OS again");
            disconnect(c); return 0;
        }
        peer = find_peer(&desc.peer_id_addr);
        if (peer < 0 && (!enrolling() || pairing_slot != c->slot)) { disconnect(c); return 0; }
        if (peer >= 0 && peers[peer].slot != c->slot) { disconnect(c); return 0; }
        if (!save_peer(&desc.peer_id_addr, c->slot)) { disconnect(c); return 0; }
        c->encrypted = c->bonded = true;
        if (c->slot==1 && b_os_bonds_managed(&desc.peer_id_addr)) b_os_bonds_success();
        neutralize(c); block_selected_held(c);
        ESP_LOGI(TAG, "PC %c bonded and encrypted (handle=%u)", 'A'+c->slot, c->handle);
        if (c->bond_trial) probe_name(c);
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        c = by_handle(event->subscribe.conn_handle);
        if (!c) return 0;
        bool *subscription;
        if (event->subscribe.attr_handle == keyboard_handle) subscription = &c->keyboard_sub;
        else if (event->subscribe.attr_handle == mouse_handle) subscription = &c->mouse_sub;
        else return 0;
        if (*subscription == event->subscribe.cur_notify) return 0;
        *subscription = event->subscribe.cur_notify;
        neutralize(c); block_selected_held(c);
        ESP_LOGI(TAG, "PC %c subscriptions keyboard=%d mouse=%d", 'A'+c->slot, c->keyboard_sub, c->mouse_sub);
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        c = by_handle(event->disconnect.conn.conn_handle);
        b_auth_gate_cancel(event->disconnect.conn.conn_handle);
        if (c) {
            if (c->bond_trial && !c->encrypted) b_os_bonds_failed(event->disconnect.reason);
            ESP_LOGI(TAG, "PC %c disconnected reason=%d", c->slot >= 0 ? 'A'+c->slot : '?', event->disconnect.reason);
            block_selected_held(c);
            memset(c, 0, sizeof(*c)); c->handle = NO_HANDLE; c->slot = -1;
        }
        return 0;
    case BLE_GAP_EVENT_TERM_FAILURE:
        c = by_handle(event->term_failure.conn_handle);
        if (c && c->disconnecting) {
            c->terminate_requested = false;
            c->disconnect_retry_at = esp_timer_get_time() + DISCONNECT_RETRY_US;
            ++c->disconnect_failures;
            ESP_LOGW(TAG, "PC %c disconnect failed status=%d; remains isolated",
                     'A'+c->slot, event->term_failure.status);
        }
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
    if (!c->release_pending && c->backlog_started &&
        esp_timer_get_time() - c->backlog_started >= INPUT_MAX_DELAY_US) {
        ++c->stale_drops;
        neutralize(c);
        block_selected_held(c);
        ESP_LOGW(TAG, "PC %c input backlog expired; releasing keys/buttons", 'A'+c->slot);
    }
    if (c->release_pending) {
        if (!c->release_started) c->release_started = esp_timer_get_time();
        if (c->key_dirty && notify(c, keyboard_handle, c->keyboard, 8)) c->key_dirty = false;
    } else {
        // Keep key-up/key-down edges across notification failures.
        for (unsigned n = 0; n < 4; ++n) {
            const uint8_t *keys = keyboard_pending_peek(&c->key_pending);
            if (!keys || !notify(c, keyboard_handle, keys, 8)) break;
            keyboard_pending_commit(&c->key_pending);
        }
        c->key_dirty = c->key_pending.count != 0;
    }
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
    if (!c->release_pending) {
        if (!c->key_dirty && !c->mouse_dirty) c->backlog_started = 0;
        else if (!c->backlog_started) c->backlog_started = esp_timer_get_time();
    }
    if (c->release_pending && !c->key_dirty && !c->mouse_dirty) {
        c->release_pending = false;
        c->switch_release = false;
        c->release_started = 0;
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
    return c && !c->disconnecting && c->switch_release && c->release_pending;
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
    if(slot==1) b_os_bonds_retry();
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
    if (b_os_bonds_forget() != ESP_OK) { reset_pending=false; ESP_LOGE(TAG,"Clearing B OS bonds failed; retry reset"); return; }
    memset(peers, 0, sizeof(peers));
    input_router_select(&router, 0);
    reset_pending = false;
    ESP_LOGI(TAG, "All A/B registrations cleared; remove '%s' from each OS before pairing again", DEVICE_NAME);
}
static void begin_profile(unsigned profile)
{
    if (reset_pending || profile_pending >= 0 || profile == os_profile_active()) return;
    profile_pending = (int)profile;
    profile_deadline = esp_timer_get_time() + 6000000;
    pairing_slot = -1;
    ble_gap_adv_stop();
    input_router_block_held(&router);
    for (unsigned i=0; i<CONNECTIONS; ++i)
        if (connections[i].handle != NO_HANDLE) neutralize(&connections[i]);
    ESP_LOGI(TAG, "Switching OS profile to %s; releasing input, then rebooting A/B connections",
             profile ? "Windows" : "Ubuntu");
}
static void finish_profile(void)
{
    if (profile_pending < 0) return;
    bool connected = false;
    for (unsigned i=0; i<CONNECTIONS; ++i) {
        connection_t *c = &connections[i];
        if (c->handle == NO_HANDLE) continue;
        connected = true;
        if (!c->release_pending || esp_timer_get_time() >= profile_deadline-3000000)
            disconnect(c);
    }
    if (connected && esp_timer_get_time() < profile_deadline) return;
    esp_err_t err = os_profile_select((unsigned)profile_pending);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OS profile save failed: %s; keeping current profile", esp_err_to_name(err));
        profile_pending = -1;
        return;
    }
    esp_restart();
}
static void log_identity_address(const char *label, const ble_addr_t *addr)
{
    ESP_LOGI(TAG, "%s type=%u address=%02X:%02X:%02X:%02X:%02X:%02X",
             label, addr->type, addr->val[5], addr->val[4], addr->val[3],
             addr->val[2], addr->val[1], addr->val[0]);
}
static void inspect_os_identity(void)
{
    ESP_LOGI(TAG, "OS identity inspection: profile=%s; fingerprints are SHA256 prefixes, no raw keys", os_profile_name());
    for (unsigned i=0;i<PEERS_MAX;++i) if(peers[i].valid && peers[i].slot==1) {
        struct ble_store_key_sec key={0};
        key.peer_addr.type=peers[i].type;
        memcpy(key.peer_addr.val,peers[i].address,6);
        log_identity_address("B saved identity", &key.peer_addr);
        struct ble_store_value_sec value={0};
        int rc=ble_store_read_peer_sec(&key,&value);
        if(rc) { ESP_LOGW(TAG, "B security metadata unavailable rc=%d",rc); continue; }
        char fingerprint[33]="unavailable";
        if(value.irk_present) {
            uint8_t digest[32]; size_t length=0;
            if(psa_crypto_init()==PSA_SUCCESS &&
               psa_hash_compute(PSA_ALG_SHA_256,value.irk,sizeof(value.irk),digest,sizeof(digest),&length)==PSA_SUCCESS && length==32) {
                for(unsigned j=0;j<16;++j) snprintf(fingerprint+2*j,3,"%02X",digest[j]);
            }
        }
        ESP_LOGI(TAG,"B security sc=%u irk_present=%u irk_fingerprint=%s",
                 value.sc,value.irk_present,value.irk_present ? fingerprint : "none");
        memset(&value,0,sizeof(value));
    }
    connection_t *c=by_slot(1);
    struct ble_gap_conn_desc desc;
    if(c && !ble_gap_conn_find(c->handle,&desc)) {
        ESP_LOGI(TAG,"B remote_name=%s name_pending=%d",c->peer_name[0] ? c->peer_name : "(unavailable)",c->name_pending);
        log_identity_address("B connected identity",&desc.peer_id_addr);
        log_identity_address("B connection address",&desc.peer_ota_addr);
        bool rpa=desc.peer_ota_addr.type==BLE_ADDR_RANDOM && (desc.peer_ota_addr.val[5]&0xc0)==0x40;
        ESP_LOGI(TAG,"B connection uses_rpa=%d encrypted=%d bonded=%d",rpa,desc.sec_state.encrypted,desc.sec_state.bonded);
    } else ESP_LOGI(TAG,"B not connected; connection address unavailable");
}
static void run_command(const char *line)
{
    if (!strcmp(line,"os retry")) {
        b_os_bonds_retry();
        ESP_LOGI(TAG,"B saved-key retry enabled; reconnect B, no bonds deleted");
        return;
    }
    if (!strcmp(line, "os ubuntu") || !strcmp(line, "os windows")) {
        begin_profile(!strcmp(line, "os windows"));
        return;
    }
    if (profile_pending >= 0) return;
    if (!strcmp(line, "os inspect")) { inspect_os_identity(); return; }
    if (!strcmp(line, "pair a") || !strcmp(line, "pair b")) {
        begin_pairing(line[5] == 'a' ? 0 : 1);
    } else if (!strcmp(line, "pair off")) {
        cancel_pairing();
    } else if (!strcmp(line, "forget a") || !strcmp(line, "forget b")) {
        if (!strcmp(line,"forget b") && b_os_bonds_forget()!=ESP_OK) { ESP_LOGE(TAG,"Clearing B OS bonds failed"); return; }
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
        ESP_LOGI(TAG, "Legacy storage bank=%u; B OS keys selected automatically by verified laptop name", os_profile_active());
        ESP_LOGI(TAG, "Selected=%c A-ready=%d B-ready=%d pairing=%c", 'A'+router.target,
                 ready(0), ready(1), enrolling() ? 'A'+pairing_slot : '-');
        portENTER_CRITICAL(&overflow_lock);
        uint32_t sources = usb_sources, reports = usb_input_reports;
        portEXIT_CRITICAL(&overflow_lock);
        ESP_LOGI(TAG, "USB sources=0x%lX reports=%lu queue=%u previous_release=%d",
                 (unsigned long)sources, (unsigned long)reports,
                 (unsigned)uxQueueMessagesWaiting(input_queue), previous_release_pending());
        for (unsigned i=0; i<CONNECTIONS; ++i) if (connections[i].handle != NO_HANDLE) {
            connection_t *c=&connections[i];
            log_connection(c);
            ESP_LOGI(TAG, "PC %c handle=%u encrypted=%d bonded=%d keyboard_sub=%d mouse_sub=%d suspended=%d release=%d switch_release=%d closing=%d terminate_pending=%d disconnect_failures=%lu",
                     c->slot >= 0 ? 'A'+c->slot : '?', c->handle, c->encrypted, c->bonded,
                     c->keyboard_sub, c->mouse_sub, c->suspended, c->release_pending,
                     c->switch_release, c->disconnecting, c->terminate_requested,
                     (unsigned long)c->disconnect_failures);
            ESP_LOGI(TAG, "PC %c pending_mouse=%u notify_failures=%lu mouse_overflows=%lu pending_keys=%u key_overflows=%lu stale_drops=%lu",
                     c->slot >= 0 ? 'A'+c->slot : '?', c->mouse_pending.count,
                     (unsigned long)c->notify_failures, (unsigned long)c->mouse_overflows,
                     c->key_pending.count, (unsigned long)c->key_overflows, (unsigned long)c->stale_drops);
        }
        for (unsigned i = 0; i < PEERS_MAX; ++i) if (peers[i].valid)
            ESP_LOGI(TAG, "Saved identity %u -> PC %c", i, 'A'+peers[i].slot);
    } else {
        ESP_LOGI(TAG, "Commands: os retry | os inspect | os ubuntu | os windows | pair a | pair b | pair off | a | b | status | forget a | forget b | help");
        ESP_LOGI(TAG, "Mouse button %d or Ctrl+Alt+F12 switches PCs. Register each OS to its PC slot.", CONFIG_INPUT_SWITCH_BUTTON);
        ESP_LOGI(TAG, "Ctrl+Alt+Shift+Left/Right enrolls A/B; Ctrl+Alt+Shift+Down cancels pairing.");
        ESP_LOGI(TAG, "Hold Ctrl+Alt+Shift+Backspace for 3 seconds to erase all A/B registrations.");
    }
}
static void process_input(const input_event_t *event)
{
    if (profile_pending >= 0) return;
    if (event->type == INPUT && event->input.keyboard) {
        const uint8_t *k = event->input.keys;
        bool ctrl = (k[0xe0/8] & ((1u<<(0xe0%8)) | (1u<<(0xe4%8)))) != 0;
        bool alt = (k[0xe2/8] & ((1u<<(0xe2%8)) | (1u<<(0xe6%8)))) != 0;
        bool ubuntu = (k[0x43/8] & (1u<<(0x43%8))) != 0; // F10
        bool windows = (k[0x44/8] & (1u<<(0x44%8))) != 0; // F11
        if (ctrl && alt && (ubuntu || windows)) {
            if (ubuntu != windows) begin_profile(windows);
            input_router_block_held(&router);
            return;
        }
    }
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
        if (!keyboard_pending_push(&c->key_pending, output.keyboard)) {
            ++c->key_overflows;
            neutralize(c);
            input_router_block_held(&router);
            flush_state(c);
            return;
        }
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
static void discard_input_backlog(void)
{
    input_event_t discard;
    // Snapshot physical state and discard older events atomically with producers.
    // This also preserves releases/removals that could not enter a full queue.
    portENTER_CRITICAL(&overflow_lock);
    for (unsigned n = 0; n < 128 && xQueueReceive(input_queue, &discard, 0); ++n) {}
    overflow = false;
    unsigned target = router.target;
    input_router_init(&router, CONFIG_INPUT_SWITCH_BUTTON);
    router.target = target;
    memcpy(router.keys, source_keys, sizeof(source_keys));
    memcpy(router.buttons, source_buttons, sizeof(source_buttons));
    portEXIT_CRITICAL(&overflow_lock);
    input_router_recover(&router);
    for (unsigned i = 0; i < CONNECTIONS; ++i)
        if (connections[i].handle != NO_HANDLE) neutralize(&connections[i]);
}
static void service_connection(connection_t *c)
{
    if (!c || c->handle == NO_HANDLE) return;
    if (c->disconnecting) { disconnect(c); return; }
    if(c->name_pending && esp_timer_get_time() >= c->name_deadline) {
        c->name_pending=false;
        ESP_LOGI(TAG,"PC B Device Name timeout; checking authentication state");
        if(!c->encrypted) start_security(c);
    }
    if (!c->encrypted && esp_timer_get_time() - c->connected_at > 60000000) {
        disconnect(c); return;
    }
    if (c->release_pending && c->release_started && (subscribed(c) || c->switch_release) &&
        esp_timer_get_time() - c->release_started >= RELEASE_TIMEOUT_US) {
        ESP_LOGW(TAG, "PC %c release timed out; isolating it so the other PC can continue", 'A'+c->slot);
        disconnect(c);
        return;
    }
    request_fast_connection(c);
    flush_state(c);
}
static void poll(struct ble_npl_event *ev)
{
    if (pairing_slot >= 0 && !enrolling()) { pairing_slot = -1; ESP_LOGI(TAG, "Pairing window expired"); }
    portENTER_CRITICAL(&overflow_lock); bool lost = overflow; overflow = false; portEXIT_CRITICAL(&overflow_lock);
    if (lost) {
        discard_input_backlog();
        ESP_LOGW(TAG, "Input queue overflow: released all keys/buttons");
    }
    // Give the selected PC first access to shared notification buffers. A
    // standby PC that is sleeping must not consume each newly available buffer.
    service_connection(by_slot(router.target));
    service_connection(by_slot(router.target ^ 1));
    for (unsigned i = 0; i < CONNECTIONS; ++i)
        if (connections[i].slot < 0) service_connection(&connections[i]);
    input_event_t event;
    for (unsigned n = 0; n < 64 && xQueueReceive(input_queue, &event, 0); ++n) {
        if (event.type == INPUT && esp_timer_get_time() - event.queued_at >= INPUT_MAX_DELAY_US) {
            discard_input_backlog();
            ESP_LOGW(TAG, "Stale USB input queue: released all keys/buttons");
            break;
        }
        if (event.type == COMMAND) run_command(event.command);
        else process_input(&event);
    }
    if (!uxQueueMessagesWaiting(input_queue) && !reset_pending &&
        input_router_reset_due(&router, esp_timer_get_time()/1000)) reset_registrations();
    finish_reset();
    finish_profile();
    if (uxQueueMessagesWaiting(input_queue)) ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &input_wakeup);
    advertise();
    portENTER_CRITICAL(&overflow_lock);
    bool no_usb_devices = usb_sources == 0;
    portEXIT_CRITICAL(&overflow_lock);
    connection_t *led_target = by_slot(router.target);
    atomic_store(&selected_leds, led_target && subscribed(led_target) ? led_target->leds & 0x1f : 0);
    status_led_update(router.target, ready(router.target) && !previous_release_pending(),
                      enrolling() ? pairing_slot : -1, no_usb_devices);
    ble_npl_callout_reset(&poll_timer, ble_npl_time_ms_to_ticks32(10));
}
static void on_reset(int reason)
{
    b_auth_gate_reset();
    ESP_LOGW(TAG, "BLE host reset reason=%d", reason);
    for (unsigned i = 0; i < CONNECTIONS; ++i) { connections[i].handle = NO_HANDLE; connections[i].slot = -1; }
    input_router_block_held(&router);
}
static void on_sync(void)
{
    b_os_bonds_install();
    int rc = ble_hs_id_set_rnd(os_profile_address());
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
    err = os_profile_init();
    if (err != ESP_OK) return err;
    err = b_os_bonds_init();
    if (err != ESP_OK) return err;
    DEVICE_NAME = os_profile_name();
    err = nvs_open(os_profile_peers_namespace(), NVS_READWRITE, &peer_nvs);
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
    b_os_bonds_install();
    ble_npl_event_init(&input_wakeup, poll, NULL);
    ble_npl_callout_init(&poll_timer, nimble_port_get_dflt_eventq(), poll, NULL);
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "BLE HID switch started. COM commands: pair a / pair b / status / help");
    return ESP_OK;
}
static void enqueue(const input_event_t *event)
{
    portENTER_CRITICAL(&overflow_lock);
    if (event->source < ROUTER_SOURCES) {
        if (event->type == REMOVE) {
            memset(source_keys[event->source], 0, HID_KEYS_BYTES);
            source_buttons[event->source] = 0;
        } else if (event->type == INPUT) {
            ++usb_input_reports;
            if (event->input.keyboard) memcpy(source_keys[event->source], event->input.keys, HID_KEYS_BYTES);
            if (event->input.mouse) source_buttons[event->source] = event->input.buttons;
        }
    }
    if (xQueueSend(input_queue, event, 0) != pdTRUE) overflow = true;
    portEXIT_CRITICAL(&overflow_lock);
    // The 10-ms callout is a retry/maintenance fallback, not an input polling delay.
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &input_wakeup);
}
void ble_switch_input(unsigned source, const hid_input_t *input)
{
    input_event_t event = {.type=INPUT, .source=source, .queued_at=esp_timer_get_time(), .input=*input}; enqueue(&event);
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
