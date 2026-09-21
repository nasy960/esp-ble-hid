#include "input_router.h"
#include <string.h>

static void aggregate(const input_router_t *r, uint8_t *keys, uint8_t *buttons)
{
    memset(keys, 0, HID_KEYS_BYTES); *buttons = 0;
    for (unsigned s = 0; s < ROUTER_SOURCES; ++s) {
        for (unsigned i = 0; i < HID_KEYS_BYTES; ++i) keys[i] |= r->keys[s][i];
        *buttons |= r->buttons[s];
    }
}
void input_router_init(input_router_t *r, unsigned switch_button)
{
    memset(r, 0, sizeof(*r));
    r->switch_mask = switch_button >= 1 && switch_button <= 8 ? 1u << (switch_button - 1) : 8;
}
void input_router_block_held(input_router_t *r)
{
    aggregate(r, r->blocked_keys, &r->blocked_buttons);
}
void input_router_select(input_router_t *r, unsigned target)
{
    r->target = target & 1;
    input_router_block_held(r);
}
static bool key(const uint8_t *keys, unsigned k) { return (keys[k / 8] >> (k % 8)) & 1; }
bool input_router_reset_due(input_router_t *r, uint64_t now_ms)
{
    if (!r->reset_held || r->reset_fired) return false;
    if (!r->reset_timing) {
        r->reset_timing = true;
        r->reset_started_ms = now_ms;
    }
    if (now_ms - r->reset_started_ms < 3000) return false;
    r->reset_fired = true;
    return true;
}
static int32_t clamp(int32_t v, int32_t low, int32_t high) { return v < low ? low : v > high ? high : v; }
void input_router_apply(input_router_t *r, unsigned source, const hid_input_t *input,
                        bool remove, bool other_ready, router_output_t *out)
{
    memset(out, 0, sizeof(*out));
    if (source >= ROUTER_SOURCES) return;
    if (remove) {
        memset(r->keys[source], 0, HID_KEYS_BYTES); r->buttons[source] = 0;
    } else {
        if (input->keyboard) memcpy(r->keys[source], input->keys, HID_KEYS_BYTES);
        if (input->mouse) r->buttons[source] = input->buttons;
    }
    uint8_t keys[HID_KEYS_BYTES], buttons;
    aggregate(r, keys, &buttons);
    bool button_down = (buttons & r->switch_mask) != 0;
    bool ctrl = key(keys, 0xe0) || key(keys, 0xe4);
    bool alt = key(keys, 0xe2) || key(keys, 0xe6);
    bool shift = key(keys, 0xe1) || key(keys, 0xe5);
    // Recognize controls before filtering held keys: registration also works
    // before any PC has paired and while normal input forwarding is blocked.
    uint8_t arrows = (key(keys, 0x50) ? 1 : 0) | (key(keys, 0x4f) ? 2 : 0) |
                     (key(keys, 0x51) ? 4 : 0);
    uint8_t pairing_down = ctrl && alt && shift ? arrows : 0;
    bool reset_chord = ctrl && alt && shift && key(keys, 0x2a);
    r->reset_held = reset_chord && !arrows;
    if (!r->reset_held) r->reset_timing = r->reset_fired = false;
    if (reset_chord) {
        input_router_block_held(r);
        pairing_down = 0; // Mixed reset/registration chords perform neither action.
    }
    if (pairing_down) {
        if (!r->pairing_down) {
            if (pairing_down == 1) out->pairing_action = ROUTER_PAIR_A;
            if (pairing_down == 2) out->pairing_action = ROUTER_PAIR_B;
            if (pairing_down == 4) out->pairing_action = ROUTER_PAIR_CANCEL;
        }
        // Suppress the entire chord until each held key has been released.
        input_router_block_held(r);
        r->pairing_down = pairing_down;
    } else if (!arrows) {
        r->pairing_down = 0;
    }
    bool shortcut_down = ctrl && alt && key(keys, 0x45);
    out->switch_requested = !reset_chord && !pairing_down && !r->pairing_down &&
                           ((button_down && !r->switch_down) ||
                            (shortcut_down && !r->shortcut_down));
    r->switch_down = button_down; r->shortcut_down = shortcut_down;
    if (out->switch_requested && other_ready) {
        input_router_select(r, r->target ^ 1);
        out->switched = true;
    }
    for (unsigned i = 0; i < HID_KEYS_BYTES; ++i) {
        r->blocked_keys[i] &= keys[i];
        keys[i] &= ~r->blocked_keys[i];
    }
    r->blocked_buttons &= buttons;
    buttons &= ~(r->blocked_buttons | r->switch_mask);
    if (shortcut_down) { // Consume fallback shortcut, including when target is unavailable.
        memset(keys, 0, sizeof(keys));
    }
    out->keyboard[0] = keys[0xe0 / 8];
    unsigned count = 0;
    for (unsigned k = 4; k < 0xe0; ++k) {
        if (key(keys, k)) {
            if (count < 6) out->keyboard[2 + count] = k;
            ++count;
        }
    }
    if (count > 6) memset(out->keyboard + 2, 1, 6); // HID ErrorRollOver, never silently truncate.
    out->mouse[0] = buttons;
    if (!remove && input->mouse && !out->switched) {
        int16_t x = clamp(input->x, -32768, 32767), y = clamp(input->y, -32768, 32767);
        out->mouse[1] = (uint16_t)x; out->mouse[2] = (uint16_t)x >> 8;
        out->mouse[3] = (uint16_t)y; out->mouse[4] = (uint16_t)y >> 8;
        out->mouse[5] = (uint8_t)(int8_t)clamp(input->wheel, -127, 127);
        out->mouse[6] = (uint8_t)(int8_t)clamp(input->pan, -127, 127);
    }
}
