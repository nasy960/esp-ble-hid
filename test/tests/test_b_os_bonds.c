#include "../main/b_os_bonds.c"
#include <assert.h>
#include <stdio.h>
struct mock_hs_cfg ble_hs_cfg;
static struct { char key[16]; struct ble_store_value_sec value; bool used; } rows[4];
static unsigned fallback;
static bool fail_commit;
static uint8_t last_os=255;
esp_err_t nvs_get_u8(nvs_handle_t h,const char *k,uint8_t *v) { (void)h;assert(!strcmp(k,"last_ok"));if(last_os==255)return ESP_ERR_NVS_NOT_FOUND;*v=last_os;return ESP_OK; }
esp_err_t nvs_set_u8(nvs_handle_t h,const char *k,uint8_t v) { (void)h;assert(!strcmp(k,"last_ok"));last_os=v;return ESP_OK; }
unsigned os_profile_active(void) { return 0; }
esp_err_t nvs_open(const char *s,int mode,nvs_handle_t *h) { (void)mode;assert(!strcmp(s,"b_os_ubuntu"));*h=1;return ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t h,const char *k,void *p,size_t *n) {
    (void)h; for(unsigned i=0;i<4;i++) if(rows[i].used && !strcmp(k,rows[i].key)) {
        assert(*n==sizeof(rows[i].value)); memcpy(p,&rows[i].value,*n);return ESP_OK;
    } return ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *k,const void *p,size_t n) {
    (void)h;assert(n==sizeof(rows[0].value));
    for(unsigned i=0;i<4;i++) if(!rows[i].used || !strcmp(k,rows[i].key)) {
        rows[i].used=true;snprintf(rows[i].key,16,"%s",k);memcpy(&rows[i].value,p,n);return ESP_OK;
    } return ESP_FAIL;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h;return fail_commit ? ESP_FAIL:ESP_OK; }
esp_err_t nvs_erase_key(nvs_handle_t h,const char *k) {
    (void)h; for(unsigned i=0;i<4;i++) if(rows[i].used && !strcmp(k,rows[i].key)) {rows[i].used=false;return ESP_OK;}
    return ESP_ERR_NVS_NOT_FOUND;
}
static int rd(int t,const union ble_store_key *k,union ble_store_value *v) { (void)t;(void)k;(void)v;++fallback;return 123; }
static int wr(int t,const union ble_store_value *v) { (void)t;(void)v;++fallback;return 123; }
static int del(int t,const union ble_store_key *k) { (void)t;(void)k;++fallback;return 123; }
int main(void) {
    ble_hs_cfg.store_read_cb=rd;ble_hs_cfg.store_write_cb=wr;ble_hs_cfg.store_delete_cb=del;
    assert(b_os_bonds_init()==ESP_OK);b_os_bonds_install();b_os_bonds_install();
    ble_addr_t addr={.type=0};memcpy(addr.val,laptop,6);
    union ble_store_key key={.sec={.peer_addr=addr}};
    union ble_store_value v={.sec={.peer_addr=addr,.ltk_present=1}},out;
    assert(read_bond(1,&key,&out)==BLE_HS_ENOENT && fallback==0);
    assert(!b_os_bonds_ready(&addr));
    assert(write_bond(1,&v)==BLE_HS_ESTORE_FAIL);
    for(unsigned os=0;os<2;os++) {
        assert(b_os_bonds_select(&addr,os?"DESKTOP-36CBTO9":"jsy",true));
        v.sec.ltk[0]=os+11;
        assert(!write_bond(1,&v));assert(!write_bond(2,&v));
    }
    // Reboot/name selection must not use whichever key was written last.
    assert(b_os_bonds_init()==ESP_OK);assert(read_bond(1,&key,&out)==BLE_HS_ENOENT);
    for(unsigned os=0;os<2;os++) {
        b_os_bonds_begin(&addr);
        assert(b_os_bonds_select(&addr,os?"DESKTOP-36CBTO9":"jsy",false));
        assert(!read_bond(1,&key,&out) && out.sec.ltk[0]==os+11);
        assert(b_os_bonds_ready(&addr));
        assert(!read_bond(2,&key,&out) && out.sec.ltk[0]==os+11);
        out.sec.ltk[0]=99;assert(write_bond(1,&out)==BLE_HS_ESTORE_FAIL);
    }
    assert(!b_os_bonds_select(&addr,"unknown",true));assert(read_bond(1,&key,&out)==BLE_HS_ENOENT);
    // Re-pair/delete Ubuntu must leave Windows intact.
    assert(b_os_bonds_select(&addr,"jsy",true));assert(!delete_bond(1,&key));
    assert(b_os_bonds_select(&addr,"DESKTOP-36CBTO9",false));assert(!read_bond(1,&key,&out) && out.sec.ltk[0]==12);
    key.sec.peer_addr.val[0]^=1;
    assert(read_bond(1,&key,&out)==123);v.sec.peer_addr=key.sec.peer_addr;
    assert(write_bond(1,&v)==123);assert(delete_bond(1,&key)==123);assert(fallback==3);
    fail_commit=true;assert(b_os_bonds_forget()!=ESP_OK);fail_commit=false;assert(b_os_bonds_forget()==ESP_OK);
    // Seed both independently, then exercise sequential reconnect attempts.
    key.sec.peer_addr=addr;v.sec.peer_addr=addr;
    for(unsigned os=0;os<2;++os) {
        assert(b_os_bonds_select(&addr,os?"DESKTOP-36CBTO9":"jsy",true));
        v.sec.ltk[0]=os+11;assert(!write_bond(1,&v));assert(!write_bond(2,&v));
    }
    b_os_bonds_success();assert(last_os==1);
    assert(b_os_bonds_init()==ESP_OK);b_os_bonds_begin(&addr);
    assert(b_os_bonds_prepare(&addr));assert(!read_bond(1,&key,&out) && out.sec.ltk[0]==12);
    b_os_bonds_failed(0x208);b_os_bonds_failed(0x213);assert(failed_mask==0); // Radio/remote disconnect.
    b_os_bonds_failed(0x23d);b_os_bonds_failed(0x23d);assert(failed_mask==2); // Duplicate events only one candidate.
    b_os_bonds_begin(&addr);assert(b_os_bonds_prepare(&addr));
    assert(!read_bond(1,&key,&out) && out.sec.ltk[0]==11);
    b_os_bonds_success();assert(last_os==0 && failed_mask==0);
    assert(b_os_bonds_init()==ESP_OK);b_os_bonds_begin(&addr);assert(b_os_bonds_prepare(&addr));
    assert(!read_bond(1,&key,&out) && out.sec.ltk[0]==11); // Persist last success, reverse OS change.
    b_os_bonds_failed(0x205);b_os_bonds_begin(&addr);assert(b_os_bonds_prepare(&addr));
    assert(!read_bond(1,&key,&out) && out.sec.ltk[0]==12);
    b_os_bonds_failed(0x206);b_os_bonds_begin(&addr);assert(!b_os_bonds_prepare(&addr));
    b_os_bonds_retry();assert(b_os_bonds_prepare(&addr));
    assert(!read_bond(1,&key,&out) && out.sec.ltk[0]==11); // Keys survived all attempts.
    out.sec.ltk[0]=99;assert(write_bond(1,&out)==BLE_HS_ESTORE_FAIL);
    assert(delete_bond(1,&key)==BLE_HS_ENOENT);
    puts("B OS bonds: independent keys, reboot selection, unknown-name gate, enrollment, A passthrough, deletion passed");
}
