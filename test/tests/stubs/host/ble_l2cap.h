#pragma once
#include "ble_sdk.h"
struct ble_l2cap_sig_update_params { uint16_t itvl_min,itvl_max,slave_latency,timeout_multiplier; };
typedef void ble_l2cap_sig_update_fn(uint16_t,int,void *);
int ble_l2cap_sig_update(uint16_t,struct ble_l2cap_sig_update_params *,ble_l2cap_sig_update_fn *,void *);
#define BLE_ERR_INV_HCI_CMD_PARMS 0x12
#define BLE_HS_HCI_ERR(x) (0x200+(x))
