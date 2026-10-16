#include "b_auth_gate.h"
#include "nimble/hci_common.h"
#include "esp_log.h"
#include <string.h>
// ESP-IDF v6.1 private NimBLE entry point. Link-time wrapper keeps the SDK
// unchanged. Save a copy: the original HCI event buffer is released on return.
int __real_ble_sm_ltk_req_rx(const struct ble_hci_ev_le_subev_lt_key_req *event);
static uint16_t waiting=0xffff;
static bool pending;
static struct ble_hci_ev_le_subev_lt_key_req saved;
void b_auth_gate_reset(void)
{ waiting=0xffff;pending=false;memset(&saved,0,sizeof(saved)); }
void b_auth_gate_begin(uint16_t handle)
{ b_auth_gate_reset();waiting=handle; }
void b_auth_gate_cancel(uint16_t handle)
{ if(waiting==handle) b_auth_gate_reset(); }
int __wrap_ble_sm_ltk_req_rx(const struct ble_hci_ev_le_subev_lt_key_req *event)
{
    const uint8_t *bytes=(const uint8_t *)&event->conn_handle;
    uint16_t handle=(uint16_t)bytes[0]|((uint16_t)bytes[1]<<8);
    if(handle!=waiting) return __real_ble_sm_ltk_req_rx(event);
    if(!pending) {
        saved=*event;pending=true;
        ESP_LOGI("b_auth_gate","PC B early LTK request deferred until Device Name response");
    }
    return 0;
}
bool b_auth_gate_resume(uint16_t handle)
{
    if(waiting!=handle) return false;
    bool replay=pending;
    struct ble_hci_ev_le_subev_lt_key_req event=saved;
    b_auth_gate_reset(); // Replayed callbacks may disconnect or re-enter.
    if(replay) {
        int rc=__real_ble_sm_ltk_req_rx(&event);
        ESP_LOGI("b_auth_gate","PC B deferred LTK request resumed after OS selection: rc=%d",rc);
    }
    return replay;
}
