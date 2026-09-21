#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { STATUS_NONE, STATUS_WAITING, STATUS_READY, STATUS_PAIRING } status_mode_t;
typedef struct {
    unsigned slot;
    status_mode_t mode;
} status_pattern_t;
status_pattern_t status_pattern_select(unsigned selected, bool selected_ready, int pairing_slot, bool no_devices);
void status_pattern_rgb(status_pattern_t pattern, uint32_t elapsed_ms,
                        uint8_t brightness, uint8_t rgb[3]);
