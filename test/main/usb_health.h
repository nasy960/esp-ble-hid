#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

// All functions run in the USB management task; never in a HID callback.
esp_err_t usb_health_init(void);
void usb_health_watch(uint8_t address, int64_t last_input, uint32_t reports, bool held);
bool usb_health_poll(void);
void usb_health_status(void);
void usb_health_reset_started(void);

void usb_health_leds(uint8_t address, uint8_t iface, uint8_t report_id, const uint8_t *data, size_t length);
