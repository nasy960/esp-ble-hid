#pragma once
int ble_hs_id_set_rnd(const unsigned char *address);
#include "sdk.h"
#include <string.h>
#define CONFIG_INPUT_SWITCH_BUTTON 4
#define ESP_ERR_NVS_NOT_FOUND 4
#define ESP_ERR_INVALID_SIZE 5
#define ESP_ERR_INVALID_STATE 6
#define NVS_READWRITE 0
typedef unsigned nvs_handle_t;
typedef int nvs_open_mode_t;
esp_err_t nvs_get_u8(nvs_handle_t, const char *, uint8_t *);
esp_err_t nvs_set_u8(nvs_handle_t, const char *, uint8_t);
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
esp_err_t nvs_flash_init(void);
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
esp_err_t nvs_commit(nvs_handle_t);
esp_err_t nvs_erase_key(nvs_handle_t, const char *);
struct ble_npl_event { int unused; };
struct ble_npl_callout { int unused; };
void *nimble_port_get_dflt_eventq(void);
void ble_npl_eventq_put(void *, struct ble_npl_event *);
void ble_npl_callout_reset(struct ble_npl_callout *, uint32_t);
uint32_t ble_npl_time_ms_to_ticks32(uint32_t);
void ble_npl_event_init(struct ble_npl_event *, void (*)(struct ble_npl_event *), void *);
void ble_npl_callout_init(struct ble_npl_callout *, void *, void (*)(struct ble_npl_event *), void *);
void nimble_port_run(void);
void nimble_port_freertos_deinit(void);
void nimble_port_freertos_init(void (*)(void *));
esp_err_t nimble_port_init(void);
unsigned uxQueueMessagesWaiting(QueueHandle_t);
struct os_mbuf { uint8_t data[32]; size_t length; };
#define OS_MBUF_PKTLEN(om) ((om)->length)
int os_mbuf_append(struct os_mbuf *, const void *, size_t);
int os_mbuf_copydata(const struct os_mbuf *, size_t, size_t, void *);
struct os_mbuf *ble_hs_mbuf_from_flat(const void *, size_t);
#define BLE_ATT_ERR_INSUFFICIENT_RES 1
#define BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN 2
#define BLE_ATT_ERR_UNLIKELY 3
#define BLE_ATT_ERR_WRITE_NOT_PERMITTED 4
#define BLE_ATT_ERR_READ_NOT_PERMITTED 5
#define BLE_ATT_F_READ 1
#define BLE_GATT_ACCESS_OP_READ_DSC 1
#define BLE_GATT_ACCESS_OP_WRITE_CHR 2
#define BLE_GATT_ACCESS_OP_READ_CHR 3
#define BLE_GATT_SVC_TYPE_PRIMARY 1
#define BLE_GATT_CHR_F_READ 1
#define BLE_GATT_CHR_F_WRITE_NO_RSP 2
#define BLE_GATT_CHR_F_WRITE_ENC 4
#define BLE_GATT_CHR_F_READ_ENC 8
#define BLE_GATT_CHR_F_NOTIFY 16
#define BLE_GATT_CHR_F_WRITE 32
typedef struct { uint8_t type; } ble_uuid_t;
typedef struct { ble_uuid_t u; uint16_t value; } ble_uuid16_t;
#define BLE_UUID16_DECLARE(n) (&(ble_uuid16_t){.value=n})
#define BLE_UUID16_INIT(n) {.value=n}
struct ble_gatt_access_ctxt { int op; struct os_mbuf *om; };
typedef int (*access_cb_t)(uint16_t,uint16_t,struct ble_gatt_access_ctxt *,void *);
struct ble_gatt_dsc_def { const ble_uuid16_t *uuid; int att_flags; access_cb_t access_cb; void *arg; };
struct ble_gatt_chr_def { const ble_uuid16_t *uuid; access_cb_t access_cb; void *arg; int flags; uint16_t *val_handle; const struct ble_gatt_dsc_def *descriptors; };
struct ble_gatt_svc_def { int type; const ble_uuid16_t *uuid; const struct ble_gatt_chr_def *characteristics; };
int ble_gatts_notify_custom(uint16_t, uint16_t, struct os_mbuf *);
int ble_gatts_count_cfg(const struct ble_gatt_svc_def *);
int ble_gatts_add_svcs(const struct ble_gatt_svc_def *);
typedef struct { uint8_t type, val[6]; } ble_addr_t;
#define BLE_ADDR_RANDOM 1
struct ble_store_key_sec { ble_addr_t peer_addr; uint8_t idx; };
struct ble_store_value_sec { ble_addr_t peer_addr; uint8_t irk[16], ltk[16]; unsigned sc, irk_present, ltk_present; };
#define BLE_ADDR_PUBLIC 0
#define BLE_STORE_OBJ_TYPE_OUR_SEC 1
#define BLE_STORE_OBJ_TYPE_PEER_SEC 2
#define BLE_HS_ESTORE_FAIL 8
union ble_store_key { struct ble_store_key_sec sec; };
union ble_store_value { struct ble_store_value_sec sec; };
typedef int ble_store_read_fn(int, const union ble_store_key *, union ble_store_value *);
typedef int ble_store_write_fn(int, const union ble_store_value *);
typedef int ble_store_delete_fn(int, const union ble_store_key *);
struct ble_gatt_error { int status; };
struct ble_gatt_attr { struct os_mbuf *om; };
int ble_gattc_read_by_uuid(uint16_t,uint16_t,uint16_t,const ble_uuid_t *,
    int (*)(uint16_t,const struct ble_gatt_error *,struct ble_gatt_attr *,void *),void *);
int ble_store_read_peer_sec(const struct ble_store_key_sec *, struct ble_store_value_sec *);
struct ble_gap_conn_desc { uint16_t conn_handle, conn_itvl, conn_latency, supervision_timeout;
    ble_addr_t peer_id_addr, peer_ota_addr; struct { bool encrypted,bonded; } sec_state; };
struct ble_hs_adv_fields { uint8_t flags; const ble_uuid16_t *uuids16; unsigned num_uuids16; bool uuids16_is_complete;
    uint16_t appearance; bool appearance_is_present; uint8_t *name; size_t name_len; bool name_is_complete; };
struct ble_gap_adv_params { int conn_mode,disc_mode,itvl_min,itvl_max; };
struct ble_gap_upd_params { int itvl_min,itvl_max,latency,supervision_timeout; };
#define BLE_GAP_EVENT_CONNECT 1
#define BLE_GAP_EVENT_ENC_CHANGE 2
#define BLE_GAP_EVENT_SUBSCRIBE 3
#define BLE_GAP_EVENT_DISCONNECT 4
#define BLE_GAP_EVENT_REPEAT_PAIRING 5
#define BLE_GAP_EVENT_ADV_COMPLETE 6
#define BLE_GAP_EVENT_CONN_UPDATE 7
#define BLE_GAP_EVENT_TERM_FAILURE 8
struct ble_gap_event {
    int type;
    struct { int status; uint16_t conn_handle; } connect, enc_change, conn_update, term_failure;
    struct { uint16_t conn_handle,attr_handle; bool cur_notify; } subscribe;
    struct { struct ble_gap_conn_desc conn; int reason; } disconnect;
    struct { uint16_t conn_handle; } repeat_pairing;
};
#define BLE_GAP_REPEAT_PAIRING_IGNORE 0
#define BLE_GAP_REPEAT_PAIRING_RETRY 1
#define BLE_HS_ADV_F_DISC_GEN 1
#define BLE_HS_ADV_F_BREDR_UNSUP 2
#define BLE_GAP_CONN_MODE_UND 0
#define BLE_GAP_DISC_MODE_GEN 0
#define BLE_HS_FOREVER -1
#define BLE_ERR_REM_USER_CONN_TERM 0x13
#define BLE_HS_EALREADY 2
#define BLE_HS_ENOTCONN 7
#define BLE_HS_ENOENT 5
#define BLE_HS_EBUSY 15
#define BLE_SM_IO_CAP_NO_IO 3
#define BLE_SM_PAIR_KEY_DIST_ENC 1
#define BLE_SM_PAIR_KEY_DIST_ID 2
struct mock_hs_cfg { ble_store_read_fn *store_read_cb; ble_store_write_fn *store_write_cb; ble_store_delete_fn *store_delete_cb; void (*sync_cb)(void); void (*reset_cb)(int); int sm_io_cap,sm_bonding,sm_sc,sm_mitm,sm_our_key_dist,sm_their_key_dist; };
extern struct mock_hs_cfg ble_hs_cfg;
bool ble_hs_synced(void);
int ble_hs_util_ensure_addr(int);
int ble_hs_id_infer_auto(int,uint8_t *);
bool ble_gap_adv_active(void);
int ble_gap_adv_set_fields(const struct ble_hs_adv_fields *);
int ble_gap_adv_start(uint8_t,const void *,int,const struct ble_gap_adv_params *,int (*)(struct ble_gap_event *,void *),void *);
int ble_gap_adv_stop(void);
int ble_gap_terminate(uint16_t,uint8_t);
int ble_gap_conn_find(uint16_t,struct ble_gap_conn_desc *);
int ble_gap_update_params(uint16_t,const struct ble_gap_upd_params *);
int ble_gap_security_initiate(uint16_t);
int ble_store_util_delete_peer(const ble_addr_t *);
int ble_store_clear(void);
void ble_svc_gap_init(void);
void ble_svc_gatt_init(void);
int ble_svc_gap_device_name_set(const char *);
int ble_svc_gap_device_appearance_set(uint16_t);
