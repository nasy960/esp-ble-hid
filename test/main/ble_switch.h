#pragma once
#include "hid_input.h"
#include "esp_err.h"
esp_err_t ble_switch_init(void);
void ble_switch_input(unsigned source, const hid_input_t *input);
void ble_switch_usb_connected(unsigned source);
void ble_switch_release_input(unsigned source);
void ble_switch_remove(unsigned source);
void ble_switch_command(const char *command);
