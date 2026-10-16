// Run the real BLE event handlers/router against a controllable radio and clock.
#include "../main/ble_switch.c"
#include <stdlib.h>

static unsigned trial_failures, trial_successes;
bool b_os_bonds_prepare(const ble_addr_t *a) { (void)a;return true; }
void b_os_bonds_failed(int r) { (void)r;++trial_failures; }
void b_os_bonds_success(void) { ++trial_successes; }
void b_os_bonds_retry(void) {}
static bool managed_b, mock_encrypted=true, mock_bonded=true;
void b_auth_gate_begin(uint16_t h) { (void)h; }
void b_auth_gate_cancel(uint16_t h) { (void)h; }
void b_auth_gate_reset(void) {}
bool b_auth_gate_resume(uint16_t h) { (void)h;return false; }
bool b_os_bonds_ready(const ble_addr_t *a) { (void)a; return true; }
bool b_os_bonds_managed(const ble_addr_t *a) { (void)a; return managed_b; }
void b_os_bonds_begin(const ble_addr_t *a) { (void)a; }
bool b_os_bonds_select(const ble_addr_t *a,const char *n,bool e) { (void)a;(void)n;(void)e;return true; }
esp_err_t b_os_bonds_forget(void) { return ESP_OK; }
static int64_t now_us;
static unsigned selected_profile, profile_saves, reboots;
static bool profile_save_fail;
static unsigned metadata_reads;
static int name_submit_rc, security_starts;
static int gap_rc, l2cap_rc;
static unsigned gap_calls,l2cap_calls,interval=12;
int ble_gattc_read_by_uuid(uint16_t h,uint16_t first,uint16_t last,const ble_uuid_t *uuid,
    int (*cb)(uint16_t,const struct ble_gatt_error *,struct ble_gatt_attr *,void *),void *arg)
{ (void)h;(void)uuid;(void)cb;(void)arg;assert(first==1 && last==0xffff);return name_submit_rc; }
int ble_store_read_peer_sec(const struct ble_store_key_sec *key,struct ble_store_value_sec *value)
{ (void)value;assert(key->peer_addr.type==1 && key->peer_addr.val[0]==42);++metadata_reads;return BLE_HS_ENOENT; }
unsigned os_profile_active(void) { return selected_profile; }
const char *os_profile_name(void) { return selected_profile ? "Windows" : "Ubuntu"; }
esp_err_t os_profile_select(unsigned p) { if(profile_save_fail)return ESP_FAIL; selected_profile=p; ++profile_saves; return ESP_OK; }
void esp_restart(void) { ++reboots; }
static bool fail_notify[2];
static int terminate_rc, terminate_calls;
static uint8_t sent_keys[2][8];
static unsigned key_sends[2], mouse_sends[2];
static uint8_t sent_mouse[2][7];
static int notification_budget;
int64_t esp_timer_get_time(void) { return now_us; }
const char *esp_err_to_name(esp_err_t e) { (void)e; return "mock"; }
int os_mbuf_append(struct os_mbuf *b,const void *p,size_t n)
{ assert(b->length+n<=sizeof(b->data));memcpy(b->data+b->length,p,n);b->length+=n;return 0; }
int os_mbuf_copydata(const struct os_mbuf *b,size_t off,size_t n,void *p)
{ if(off+n>b->length)return -1;memcpy(p,b->data+off,n);return 0; }
struct os_mbuf *ble_hs_mbuf_from_flat(const void *p,size_t n)
{ static struct os_mbuf b; b.length=0;os_mbuf_append(&b,p,n);return &b; }
int ble_gatts_notify_custom(uint16_t h,uint16_t attr,struct os_mbuf *b)
{
    assert(h<2);
    if(fail_notify[h] || notification_budget==0) return BLE_HS_EBUSY;
    if(notification_budget>0) --notification_budget;
    if(attr==keyboard_handle) { memcpy(sent_keys[h],b->data,8);++key_sends[h]; }
    if(attr==mouse_handle) { memcpy(sent_mouse[h],b->data,7);++mouse_sends[h]; }
    return 0;
}
int ble_gap_terminate(uint16_t h,uint8_t reason)
{ assert(h<2 && reason==BLE_ERR_REM_USER_CONN_TERM);++terminate_calls;return terminate_rc; }
int ble_gap_conn_find(uint16_t h,struct ble_gap_conn_desc *d)
{ *d=(struct ble_gap_conn_desc){.conn_handle=h,.conn_itvl=interval,.sec_state={mock_encrypted,mock_bonded}};return 0; }
int ble_gap_update_params(uint16_t h,const struct ble_gap_upd_params *p) { (void)h;assert(p->itvl_min==6 && p->itvl_max==6 && p->latency==0 && p->supervision_timeout==400);++gap_calls;return gap_rc; }
int ble_l2cap_sig_update(uint16_t h,struct ble_l2cap_sig_update_params *p,ble_l2cap_sig_update_fn *cb,void *arg)
{ (void)h;(void)cb;(void)arg;assert(p->itvl_min==6 && p->itvl_max==6 && p->slave_latency==0 && p->timeout_multiplier==400);++l2cap_calls;return l2cap_rc; }
int ble_gap_security_initiate(uint16_t h) { (void)h;++security_starts;return 0; }
esp_err_t nvs_set_blob(nvs_handle_t h,const char *k,const void *p,size_t n) { (void)h;(void)k;(void)p;(void)n;return 0; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h;return 0; }
esp_err_t nvs_erase_key(nvs_handle_t h,const char *k) { (void)h;(void)k;return 0; }
int ble_store_util_delete_peer(const ble_addr_t *p) { (void)p;return 0; }
int ble_store_clear(void) { return 0; }
int ble_gap_adv_stop(void) { return 0; }
bool ble_gap_adv_active(void) { return true; }
bool ble_hs_synced(void) { return true; }
int ble_gap_adv_set_fields(const struct ble_hs_adv_fields *p) { (void)p;return 0; }
int ble_gap_adv_start(uint8_t a,const void *b,int c,const struct ble_gap_adv_params *d,int (*e)(struct ble_gap_event *,void *),void *f)
{ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;return 0; }
BaseType_t xQueueReceive(QueueHandle_t q,void *p,unsigned t) { (void)q;(void)p;(void)t;return 0; }
unsigned uxQueueMessagesWaiting(QueueHandle_t q) { (void)q;return 0; }
void *nimble_port_get_dflt_eventq(void) { return NULL; }
void ble_npl_eventq_put(void *q,struct ble_npl_event *e) { (void)q;(void)e; }
void ble_npl_callout_reset(struct ble_npl_callout *c,uint32_t t) { (void)c;(void)t; }
uint32_t ble_npl_time_ms_to_ticks32(uint32_t t) { return t; }
void status_led_update(unsigned a,bool b,int c,bool d) { (void)a;(void)b;(void)c;(void)d; }
static void setup(void)
{
    trial_failures=trial_successes=0;managed_b=false;mock_encrypted=mock_bonded=true;
    name_submit_rc=0;security_starts=0;gap_rc=0;l2cap_rc=0;gap_calls=l2cap_calls=0;interval=12;
    profile_pending=-1;selected_profile=0;profile_saves=0;reboots=0;profile_save_fail=false;
    memset(connections,0,sizeof(connections));
    memset(sent_keys,0,sizeof(sent_keys));memset(key_sends,0,sizeof(key_sends));
    memset(sent_mouse,0,sizeof(sent_mouse));memset(mouse_sends,0,sizeof(mouse_sends));
    memset(peers,0,sizeof(peers));notification_budget=-1;
    memset(fail_notify,0,sizeof(fail_notify));
    now_us=1000000;terminate_rc=0;terminate_calls=0;pairing_slot=-1;reset_pending=false;overflow=false;
    keyboard_handle=10;mouse_handle=11;
    input_router_init(&router,4);
    for(unsigned i=0;i<2;++i) connections[i]=(connection_t){.handle=i,.slot=i,
        .encrypted=true,.bonded=true,.keyboard_sub=true,.mouse_sub=true,.params_requested=true};
}
static void key_input(unsigned usage)
{
    input_event_t e={.type=INPUT,.source=0,.input={.keyboard=true}};
    if(usage) e.input.keys[usage/8]|=1u<<(usage%8);
    process_input(&e);
}
static void toggle(bool down)
{
    input_event_t e={.type=INPUT,.source=1,.input={.mouse=true,.buttons=down?8:0}};
    process_input(&e);
}
static void control(unsigned slot,uint8_t value)
{
    struct os_mbuf b={.data={value},.length=1};
    struct ble_gatt_access_ctxt ctx={.op=BLE_GATT_ACCESS_OP_WRITE_CHR,.om=&b};
    assert(access_report(slot,0,&ctx,(void *)ATTR_CONTROL)==0);
}
static void unsub(unsigned slot,uint16_t attr,bool enabled)
{
    struct ble_gap_event e={.type=BLE_GAP_EVENT_SUBSCRIBE,
        .subscribe={.conn_handle=slot,.attr_handle=attr,.cur_notify=enabled}};
    gap_event(&e,NULL);
}
static void disconnected(unsigned slot)
{
    struct ble_gap_event e={.type=BLE_GAP_EVENT_DISCONNECT,.disconnect={.conn={.conn_handle=slot}}};
    gap_event(&e,NULL);
}
static void inactive_sleep(void)
{
    setup();key_input(4);assert(sent_keys[0][2]==4);
    control(1,0);key_input(4);
    assert(sent_keys[0][2]==4); // Standby sleep must not release an active PC's held key.
    unsub(1,keyboard_handle,false);key_input(4);assert(sent_keys[0][2]==4);
    disconnected(1);key_input(4);assert(sent_keys[0][2]==4);
    key_input(0);assert(sent_keys[0][2]==0);
    // Fresh disconnect, subscription, encryption and resume events are isolated too.
    setup();key_input(4);disconnected(1);key_input(4);assert(sent_keys[0][2]==4);
    setup();key_input(4);unsub(1,mouse_handle,false);key_input(4);assert(sent_keys[0][2]==4);
    unsub(1,mouse_handle,true);key_input(4);assert(sent_keys[0][2]==4);
    peers[0]=(peer_t){.valid=1,.slot=1};
    struct ble_gap_event e={.type=BLE_GAP_EVENT_ENC_CHANGE,.enc_change={.conn_handle=1}};
    gap_event(&e,NULL);key_input(4);assert(sent_keys[0][2]==4);
    control(1,0);control(1,1);key_input(4);assert(sent_keys[0][2]==4);
    input_event_t mouse={.type=INPUT,.source=1,.input={.mouse=true,.buttons=1,.x=7}};
    process_input(&mouse);control(1,0);process_input(&mouse);
    assert(sent_mouse[0][0]==1 && sent_mouse[0][1]==7);
}
static void delayed_disconnect(void)
{
    setup();key_input(4);fail_notify[0]=true;terminate_rc=BLE_HS_EBUSY;
    toggle(true);assert(router.target==1);toggle(false);
    poll(NULL);assert(previous_release_pending());
    now_us+=3100000;poll(NULL);
    assert(!previous_release_pending()); // Old PC cannot hold the new PC hostage.
    key_input(0);key_input(5);assert(sent_keys[1][2]==5);
}
static void selected_sleep(void)
{
    setup();key_input(4);control(0,0);
    assert(!ready(0) && ready(1));
    toggle(true);assert(router.target==1 && connections[0].disconnecting);
    assert(!previous_release_pending());
    poll(NULL);toggle(false);key_input(0);key_input(5);
    assert(sent_keys[1][2]==5 && terminate_calls==1);
    // A late disconnect of the old PC must not release B's held key.
    disconnected(0);key_input(5);assert(sent_keys[1][2]==5);
    // Reconnect is initially neutral, cannot steal selection or replay old keys.
    peers[0]=(peer_t){.valid=1,.slot=0};
    struct ble_gap_event connect={.type=BLE_GAP_EVENT_CONNECT,.connect={.conn_handle=0}};
    gap_event(&connect,NULL);
    struct ble_gap_event encrypt={.type=BLE_GAP_EVENT_ENC_CHANGE,.enc_change={.conn_handle=0}};
    gap_event(&encrypt,NULL);
    unsub(0,keyboard_handle,true);unsub(0,mouse_handle,true);poll(NULL);
    assert(ready(0) && router.target==1 && sent_keys[0][2]==0);
    key_input(5);assert(sent_keys[1][2]==5);
    key_input(0);toggle(true);assert(router.target==0);
}
static void retry_disconnect(void)
{
    delayed_disconnect();
    assert(connections[0].disconnecting && !ready(0) && terminate_calls==1);
    unsigned old_sends=key_sends[0];
    fail_notify[0]=false; // Even a recovering old link cannot accept normal input now.
    for(unsigned i=0;i<50;++i) { now_us+=10000;poll(NULL); }
    assert(terminate_calls==1 && key_sends[0]==old_sends && sent_keys[1][2]==5);
    now_us+=500000;terminate_rc=0;poll(NULL);
    assert(terminate_calls==2 && connections[0].terminate_requested);
    now_us+=5000000;poll(NULL);assert(terminate_calls==2 && ready(1));
    struct ble_gap_event e={.type=BLE_GAP_EVENT_TERM_FAILURE,
                           .term_failure={.conn_handle=0,.status=42}};
    gap_event(&e,NULL);assert(!connections[0].terminate_requested);
    now_us+=1000000;terminate_rc=BLE_HS_EALREADY;poll(NULL);
    assert(terminate_calls==3 && connections[0].terminate_requested && ready(1));
    gap_event(&e,NULL);now_us+=1000000;terminate_rc=BLE_HS_ENOTCONN;poll(NULL);
    assert(connections[0].handle==NO_HANDLE && ready(1));
}
static void active_priority(void)
{
    setup();input_router_select(&router,1);
    neutralize(&connections[0]);neutralize(&connections[1]);
    notification_budget=1;poll(NULL);
    assert(key_sends[1]==1 && key_sends[0]==0);
    notification_budget=1;poll(NULL);
    assert(mouse_sends[1]==1 && ready(1));
    notification_budget=-1;poll(NULL);assert(ready(0));
}
static void suspended_waiting_release(void)
{
    setup();fail_notify[0]=true;toggle(true);toggle(false);poll(NULL);
    control(0,0);assert(previous_release_pending());
    now_us+=3000000;poll(NULL);
    assert(!previous_release_pending() && connections[0].disconnecting && ready(1));
    key_input(6);assert(sent_keys[1][2]==6);
}
static void keyboard_switch_from_sleep(void)
{
    setup();control(0,0);
    input_event_t chord={.type=INPUT,.source=0,.input={.keyboard=true}};
    const unsigned keys[]={0xe0,0xe2,0x45};
    for(unsigned i=0;i<3;++i) chord.input.keys[keys[i]/8]|=1u<<(keys[i]%8);
    process_input(&chord);assert(router.target==1);
    poll(NULL);key_input(0);key_input(8);assert(sent_keys[1][2]==8);
    // A suspended destination must still be rejected without disturbing input.
    toggle(true);assert(router.target==1);key_input(8);assert(sent_keys[1][2]==8);
    unsigned sends=key_sends[1];
    unsub(1,keyboard_handle,true);control(1,1); // Duplicate state notifications.
    key_input(8);assert(key_sends[1]==sends && sent_keys[1][2]==8);
}
static void selected_failure_can_switch(void)
{
    setup();fail_notify[0]=true;neutralize(&connections[0]);poll(NULL);
    now_us+=3000000;poll(NULL);
    assert(connections[0].disconnecting && ready(1));
    toggle(true);poll(NULL);toggle(false);key_input(7);
    assert(router.target==1 && sent_keys[1][2]==7);
}
static void profile_switching(void)
{
    setup();key_input(4);begin_profile(0);assert(profile_pending==-1);
    input_event_t chord={.type=INPUT,.source=0,.input={.keyboard=true}};
    unsigned usages[]={0xe0,0xe2,0x44};
    for(unsigned i=0;i<3;++i) chord.input.keys[usages[i]/8]|=1u<<(usages[i]%8);
    process_input(&chord);assert(profile_pending==1 && profile_saves==0);
    flush_state(&connections[0]);flush_state(&connections[1]);
    assert(sent_keys[0][2]==0); // Release before disconnect/reboot.
    key_input(5);assert(sent_keys[0][2]==0);
    finish_profile();assert(terminate_calls==2 && reboots==0);
    connections[0].handle=NO_HANDLE;connections[1].handle=NO_HANDLE;
    finish_profile();assert(profile_saves==1 && reboots==1 && selected_profile==1);
    setup();fail_notify[0]=true;begin_profile(1);
    now_us+=6000000;finish_profile();assert(reboots==1); // Bounded failed release/disconnect.
    setup();begin_profile(1);profile_save_fail=true;now_us+=6000000;
    finish_profile();assert(reboots==0 && selected_profile==0 && profile_pending==-1);
}
static void lock_leds(void)
{
    setup();
    struct os_mbuf data={.length=1,.data={2}};
    struct ble_gatt_access_ctxt ctxt={.op=BLE_GATT_ACCESS_OP_WRITE_CHR,.om=&data};
    assert(access_report(0,0,&ctxt,(void *)ATTR_LEDS)==0);
    data.data[0]=1;assert(access_report(1,0,&ctxt,(void *)ATTR_LEDS)==0);
    poll(NULL);assert(ble_switch_leds()==2);
    switch_to(1);poll(NULL);assert(ble_switch_leds()==1);
    connections[1].handle=NO_HANDLE;poll(NULL);assert(ble_switch_leds()==0);
}
static void identity_inspection(void)
{
    setup();peers[0]=(peer_t){.valid=1,.slot=0};
    peers[1]=(peer_t){.valid=1,.slot=1,.type=1,.address={42}};
    unsigned before=metadata_reads;
    run_command("os inspect");
    assert(metadata_reads==before+1 && profile_pending==-1 && profile_saves==0 && reboots==0);
    assert(peers[0].valid && peers[1].address[0]==42);
}
static void name_inspection(void)
{
    setup();connection_t *c=&connections[1];c->encrypted=false;mock_encrypted=mock_bonded=false;
    probe_name(c);assert(c->name_pending && security_starts==0);
    struct os_mbuf data={.length=16,.data="DESKTOP-36CBTO9"}; // Windows includes a trailing NUL.
    struct ble_gatt_attr attr={.om=&data};struct ble_gatt_error error={0};
    uint32_t token=c->name_token;
    name_read_done(1,&error,&attr,(void *)(uintptr_t)(token+1));assert(c->name_pending);
    name_read_done(1,&error,&attr,(void *)(uintptr_t)token);
    assert(!c->name_pending && security_starts==1 && !strcmp(c->peer_name,"DESKTOP-36CBTO9"));
    name_read_done(1,&error,&attr,(void *)(uintptr_t)token);assert(security_starts==1);
    probe_name(c);now_us+=2000000;service_connection(c);
    assert(!c->name_pending && security_starts==2);
    name_submit_rc=1;probe_name(c);assert(!c->name_pending && security_starts==3);
}
static void early_authentication(void)
{
    setup();managed_b=true;mock_encrypted=mock_bonded=false;
    connection_t *c=&connections[1];c->encrypted=c->bonded=false;
    probe_name(c);
    struct ble_gap_event event={.type=BLE_GAP_EVENT_ENC_CHANGE,.enc_change={.conn_handle=1,.status=5}};
    gap_event(&event,NULL);
    assert(c->name_pending && !c->disconnecting && !ready(1) && terminate_calls==0);
    struct os_mbuf data={.length=3,.data="jsy"};
    struct ble_gatt_attr attr={.om=&data};struct ble_gatt_error error={0};
    name_read_done(1,&error,&attr,(void *)(uintptr_t)c->name_token);
    assert(!c->name_pending && security_starts==1 && !c->disconnecting && !ready(1));
    // A genuinely successful early event is revalidated after name completes.
    setup();managed_b=true;c=&connections[1];c->encrypted=c->bonded=false;
    peers[0]=(peer_t){.valid=1,.slot=1};
    probe_name(c);event.enc_change.status=0;gap_event(&event,NULL);
    assert(!ready(1));
    name_read_done(1,&error,&attr,(void *)(uintptr_t)c->name_token);
    assert(c->encrypted && c->bonded && !c->disconnecting && security_starts==0);
}
static void saved_key_connection(void)
{
    setup();managed_b=true;mock_encrypted=mock_bonded=false;
    connection_t *c=&connections[1];c->encrypted=c->bonded=false;c->bond_trial=true;
    // Normal reconnection has no name prerequisite and no enrollment.
    start_security(c);assert(security_starts==1 && !c->disconnecting);
    struct ble_gap_event e={.type=BLE_GAP_EVENT_ENC_CHANGE,.enc_change={.conn_handle=1,.status=0x23d}};
    gap_event(&e,NULL);assert(c->disconnecting && trial_failures==1);
    setup();managed_b=true;c=&connections[1];c->encrypted=c->bonded=false;c->bond_trial=true;
    peers[0]=(peer_t){.valid=1,.slot=1};e.enc_change.status=0;
    gap_event(&e,NULL);assert(c->encrypted && c->bonded && c->name_pending && trial_successes==1);
    int starts=security_starts;
    struct ble_gatt_error error={.status=1};
    name_read_done(1,&error,NULL,(void *)(uintptr_t)c->name_token);
    assert(c->encrypted && !c->disconnecting && security_starts==starts); // Diagnostic failure cannot tear down working auth.
}
static void interval_fallback(void)
{
    setup(); connection_t *c=&connections[1];c->params_requested=false;interval=6;
    request_fast_connection(c);assert(c->params_requested && gap_calls==0); // Already 7.5ms.
    c->params_requested=false;interval=12;gap_rc=530; // Request even on a working 15ms link.
    request_fast_connection(c);assert(gap_calls==1 && c->params_l2cap && !c->params_requested);
    request_fast_connection(c);assert(l2cap_calls==0); // Backoff.
    now_us+=2000000;request_fast_connection(c);assert(l2cap_calls==1 && c->params_requested);
    uint32_t token=c->params_token;
    params_l2cap_done(1,1,(void *)(uintptr_t)(token+1));assert(c->params_requested); // Reused/stale callback.
    params_l2cap_done(1,1,(void *)(uintptr_t)token);assert(!c->params_requested);
    now_us+=5000000;l2cap_rc=1;request_fast_connection(c);assert(l2cap_calls==2);
    now_us+=5000000;request_fast_connection(c);assert(l2cap_calls==2 && gap_calls==1); // Three total attempts.
    setup();c=&connections[1];c->params_requested=false;interval=36;
    request_fast_connection(c);assert(gap_calls==1 && c->params_requested && l2cap_calls==0);
}
int main(int argc,char **argv)
{
    if(argc>1 && !strcmp(argv[1],"timeout")) delayed_disconnect();
    else { saved_key_connection();early_authentication();interval_fallback();inactive_sleep();selected_sleep();retry_disconnect();active_priority();
           suspended_waiting_release();keyboard_switch_from_sleep();selected_failure_can_switch();profile_switching();lock_leds();identity_inspection();name_inspection(); }
    puts("BLE standby/selected sleep, reconnect, release timeout and disconnect retry tests passed");
}
