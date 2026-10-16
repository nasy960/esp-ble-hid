#include "../main/b_auth_gate.c"
#include <assert.h>
#include <stdio.h>
static unsigned calls;
static struct ble_hci_ev_le_subev_lt_key_req received;
int __real_ble_sm_ltk_req_rx(const struct ble_hci_ev_le_subev_lt_key_req *event)
{ ++calls;received=*event;return 0; }
int main(void)
{
    struct ble_hci_ev_le_subev_lt_key_req event={.conn_handle=1,.rand=123,.div=45};
    b_auth_gate_begin(1);
    __wrap_ble_sm_ltk_req_rx(&event);assert(!calls);
    event.rand=999; // Original HCI buffer can be freed/reused.
    __wrap_ble_sm_ltk_req_rx(&event);assert(!calls);
    event.conn_handle=2;__wrap_ble_sm_ltk_req_rx(&event);assert(calls==1); // A unaffected.
    b_auth_gate_cancel(2);assert(b_auth_gate_resume(1));assert(calls==2 && received.rand==123 && received.div==45);
    assert(!b_auth_gate_resume(1));
    event.conn_handle=1;__wrap_ble_sm_ltk_req_rx(&event);assert(calls==3); // Name selected: no gate.
    b_auth_gate_begin(1);__wrap_ble_sm_ltk_req_rx(&event);b_auth_gate_cancel(1);
    assert(!b_auth_gate_resume(1) && calls==3); // Disconnect/timeout must discard.
    b_auth_gate_begin(1);assert(!b_auth_gate_resume(1)); // Name before LTK.
    __wrap_ble_sm_ltk_req_rx(&event);assert(calls==4);
    b_auth_gate_begin(1);__wrap_ble_sm_ltk_req_rx(&event);b_auth_gate_reset();
    assert(!b_auth_gate_resume(1) && calls==4);
    puts("B auth gate: event copy, replay once, A passthrough, cancellation and reset passed");
}
