#pragma once
#include "hid_input.h"
#define ROUTER_SOURCES 8

typedef struct {
    uint8_t keys[ROUTER_SOURCES][HID_KEYS_BYTES], buttons[ROUTER_SOURCES];
    uint8_t blocked_keys[HID_KEYS_BYTES], blocked_buttons;
    uint8_t switch_mask;
    bool switch_down, shortcut_down;
    uint8_t pairing_down;
    bool reset_held, reset_timing, reset_fired;
    uint64_t reset_started_ms;
    unsigned target;
} input_router_t;
typedef enum {
    ROUTER_PAIR_NONE, ROUTER_PAIR_A, ROUTER_PAIR_B, ROUTER_PAIR_CANCEL,
} router_pair_action_t;
typedef struct {
    uint8_t keyboard[8], mouse[7];
    bool switched, switch_requested;
    router_pair_action_t pairing_action;
} router_output_t;
void input_router_init(input_router_t *r, unsigned switch_button);
void input_router_block_held(input_router_t *r);
void input_router_select(input_router_t *r, unsigned target);
bool input_router_reset_due(input_router_t *r, uint64_t now_ms);
void input_router_apply(input_router_t *r, unsigned source, const hid_input_t *input,
                        bool remove, bool other_ready, router_output_t *out);
