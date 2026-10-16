// Device-specific automatic bond selection for the verified dual-boot laptop.
// Names select credentials; they never replace BLE authentication.
#include "b_os_bonds.h"
#include "os_profile.h"
#include "host/ble_store.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>
static const uint8_t laptop[6] = {0x23,0x78,0xc7,0x8e,0x19,0xac};
static nvs_handle_t db;
static int selected = -1;
static bool allow_write;
static unsigned failed_mask;
static int preferred;
static bool trial, key_used, failure_recorded;
static ble_store_read_fn *base_read;
static ble_store_write_fn *base_write;
static ble_store_delete_fn *base_delete;
bool b_os_bonds_managed(const ble_addr_t *addr)
{ return addr->type == BLE_ADDR_PUBLIC && !memcmp(addr->val,laptop,6); }
void b_os_bonds_begin(const ble_addr_t *addr)
{ if (b_os_bonds_managed(addr)) { selected=-1; allow_write=false; trial=false; key_used=false; failure_recorded=false; } }
bool b_os_bonds_select(const ble_addr_t *addr, const char *name, bool enroll)
{
    if (!b_os_bonds_managed(addr)) return true;
    int previous=selected;
    trial=false;
    selected = !strcmp(name,"jsy") ? 0 : !strcmp(name,"DESKTOP-36CBTO9") ? 1 : -1;
    allow_write = selected >= 0 && enroll;
    if(selected>=0 && selected!=previous) ESP_LOGI("b_os_bonds","PC B OS=%s; using separate saved keys",selected ? "Windows" : "Ubuntu");
    return selected >= 0;
}
static bool security(int type)
{ return type==BLE_STORE_OBJ_TYPE_OUR_SEC || type==BLE_STORE_OBJ_TYPE_PEER_SEC; }
static const char *record(int type)
{
    if(selected==0) return type==BLE_STORE_OBJ_TYPE_OUR_SEC ? "u_our_v1" : "u_peer_v1";
    return type==BLE_STORE_OBJ_TYPE_OUR_SEC ? "w_our_v1" : "w_peer_v1";
}
static int read_bond(int type,const union ble_store_key *key,union ble_store_value *value)
{
    if(!security(type) || !b_os_bonds_managed(&key->sec.peer_addr))
        return base_read(type,key,value);
    // Never fall back to the legacy single-OS bond for this address.
    if(selected<0 || key->sec.idx) return BLE_HS_ENOENT;
    memset(value,0,sizeof(*value));
    size_t size=sizeof(value->sec);
    esp_err_t err=nvs_get_blob(db,record(type),&value->sec,&size);
    if(err==ESP_ERR_NVS_NOT_FOUND) return BLE_HS_ENOENT;
    if(err!=ESP_OK || size!=sizeof(value->sec) || !b_os_bonds_managed(&value->sec.peer_addr)) return BLE_HS_ESTORE_FAIL;
    if(type==BLE_STORE_OBJ_TYPE_OUR_SEC && trial) key_used=true;
    return 0;
}
static int write_bond(int type,const union ble_store_value *value)
{
    if(!security(type) || !b_os_bonds_managed(&value->sec.peer_addr)) return base_write(type,value);
    if(selected<0) return BLE_HS_ESTORE_FAIL;
    // Reconnection may persist counters. Without an enrollment window, only
    // updates retaining the existing encryption key are permitted.
    if(!allow_write) {
        union ble_store_key key={0}; key.sec.peer_addr=value->sec.peer_addr;
        union ble_store_value old;
        if(read_bond(type,&key,&old) || old.sec.ltk_present!=value->sec.ltk_present ||
           memcmp(old.sec.ltk,value->sec.ltk,sizeof(old.sec.ltk))) return BLE_HS_ESTORE_FAIL;
    }
    esp_err_t err=nvs_set_blob(db,record(type),&value->sec,sizeof(value->sec));
    if(err==ESP_OK) err=nvs_commit(db);
    return err==ESP_OK ? 0 : BLE_HS_ESTORE_FAIL;
}
static int delete_bond(int type,const union ble_store_key *key)
{
    if(!security(type) || !b_os_bonds_managed(&key->sec.peer_addr)) return base_delete(type,key);
    if(selected<0 || !allow_write || key->sec.idx) return BLE_HS_ENOENT;
    esp_err_t err=nvs_erase_key(db,record(type));
    if(err==ESP_ERR_NVS_NOT_FOUND) return BLE_HS_ENOENT;
    if(err==ESP_OK) err=nvs_commit(db);
    return err==ESP_OK ? 0 : BLE_HS_ESTORE_FAIL;
}
esp_err_t b_os_bonds_forget(void)
{
    const char *keys[]={"u_our_v1","u_peer_v1","w_our_v1","w_peer_v1"};
    for(unsigned i=0;i<4;++i) {
        esp_err_t err=nvs_erase_key(db,keys[i]);
        if(err!=ESP_OK && err!=ESP_ERR_NVS_NOT_FOUND) return err;
    }
    b_os_bonds_retry();
    return nvs_commit(db);
}
esp_err_t b_os_bonds_init(void)
{
    selected=-1; allow_write=false; failed_mask=0;preferred=0;trial=key_used=failure_recorded=false;
    esp_err_t err=nvs_open(os_profile_active()?"b_os_windows":"b_os_ubuntu",NVS_READWRITE,&db);
    if(err!=ESP_OK) return err;
    uint8_t last=0;
    err=nvs_get_u8(db,"last_ok",&last);
    if(err!=ESP_OK && err!=ESP_ERR_NVS_NOT_FOUND) return err;
    if(err==ESP_OK && last<2) preferred=last;
    return ESP_OK;
}
void b_os_bonds_install(void)
{
    if(ble_hs_cfg.store_read_cb==read_bond) return;
    base_read=ble_hs_cfg.store_read_cb; base_write=ble_hs_cfg.store_write_cb; base_delete=ble_hs_cfg.store_delete_cb;
    ble_hs_cfg.store_read_cb=read_bond; ble_hs_cfg.store_write_cb=write_bond; ble_hs_cfg.store_delete_cb=delete_bond;
}

bool b_os_bonds_ready(const ble_addr_t *addr)
{
    if(!b_os_bonds_managed(addr)) return true;
    union ble_store_key key={0}; key.sec.peer_addr=*addr;
    union ble_store_value value;
    return read_bond(BLE_STORE_OBJ_TYPE_OUR_SEC,&key,&value)==0 && value.sec.ltk_present;
}

void b_os_bonds_retry(void)
{ failed_mask=0;trial=key_used=failure_recorded=false; }
bool b_os_bonds_prepare(const ble_addr_t *addr)
{
    if(!b_os_bonds_managed(addr)) return true;
    allow_write=false;trial=false;key_used=false;failure_recorded=false;
    for(unsigned n=0;n<2;++n) {
        selected=preferred ^ n;
        if(failed_mask & (1u<<selected)) continue;
        if(b_os_bonds_ready(addr)) {
            trial=true;
            ESP_LOGI("b_os_bonds","PC B trying saved %s key; name lookup follows authentication",selected?"Windows":"Ubuntu");
            return true;
        }
    }
    selected=-1;
    ESP_LOGW("b_os_bonds","PC B no untried saved key; stopped. Use os retry to retry, pair b only to enroll");
    return false;
}
void b_os_bonds_failed(int reason)
{
    // HCI Authentication Failure / PIN or Key Missing / MIC Failure only.
    // Remote termination (0x213), timeout, resource errors are not key evidence.
    if(!trial || !key_used || failure_recorded || selected<0 ||
       (reason!=0x205 && reason!=0x206 && reason!=0x23d)) return;
    failed_mask |= 1u<<selected;failure_recorded=true;
    ESP_LOGW("b_os_bonds","PC B %s key authentication failed (reason=%d); next reconnect may try other saved key",selected?"Windows":"Ubuntu",reason);
}
void b_os_bonds_success(void)
{
    if(selected<0) return;
    failed_mask=0;trial=false;key_used=false;failure_recorded=false;
    uint8_t last=0xff;
    esp_err_t err=nvs_get_u8(db,"last_ok",&last);
    if(err==ESP_OK && last==selected) { preferred=selected; return; }
    if(err==ESP_OK || err==ESP_ERR_NVS_NOT_FOUND) {
        err=nvs_set_u8(db,"last_ok",selected);
        if(err==ESP_OK) err=nvs_commit(db);
    }
    preferred=selected;
    if(err!=ESP_OK) ESP_LOGW("b_os_bonds","Could not persist last successful OS: %d",err);
}
