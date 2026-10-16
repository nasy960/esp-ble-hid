#pragma once
#include <stdbool.h>
#include "esp_err.h"
esp_err_t status_led_init(void);
void status_led_boot_complete(void);
void status_led_update(unsigned selected, bool selected_ready, int pairing_slot, bool no_devices);
