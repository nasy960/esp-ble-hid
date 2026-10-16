#pragma once
#include <stdint.h>
struct ble_hci_ev_le_subev_lt_key_req { uint8_t subev_code;uint16_t conn_handle;uint64_t rand;uint16_t div; } __attribute__((packed));
