#include "status_pattern.h"

status_pattern_t status_pattern_select(unsigned selected, bool selected_ready, int pairing_slot, bool no_devices)
{
    return (status_pattern_t) {
        .slot = pairing_slot >= 0 ? (unsigned)pairing_slot & 1 : selected & 1,
        .mode = pairing_slot >= 0 ? STATUS_PAIRING : no_devices ? STATUS_NONE :
                selected_ready ? STATUS_READY : STATUS_WAITING,
    };
}
void status_pattern_rgb(status_pattern_t p, uint32_t elapsed_ms,
                        uint8_t brightness, uint8_t rgb[3])
{
    uint32_t period = p.mode == STATUS_PAIRING ? 250 : p.mode == STATUS_READY ? 1000 : 2000;
    uint32_t on_time = p.mode == STATUS_PAIRING ? 125 : p.mode == STATUS_READY ? 500 : 100;
    bool on = p.mode != STATUS_NONE && elapsed_ms % period < on_time;
    rgb[0] = p.mode == STATUS_NONE ? brightness : 0;
    rgb[1] = on && p.slot == 0 ? brightness : 0; // A: green
    rgb[2] = on && p.slot == 1 ? brightness : 0; // B: blue
}
