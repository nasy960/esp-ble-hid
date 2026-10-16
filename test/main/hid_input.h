#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HID_LAYOUT_REPORTS 16
#define HID_LAYOUT_FIELDS 128
#define HID_INPUT_BYTES 64
#define HID_KEYS_BYTES 32

typedef struct {
    uint16_t offset, count;
    uint8_t size, report, kind;
    uint16_t usage;
    int32_t logical_min;
} hid_field_t;
typedef struct {
    uint8_t ids[HID_LAYOUT_REPORTS];
    uint16_t bits[HID_LAYOUT_REPORTS];
    uint16_t output_bits[HID_LAYOUT_REPORTS], led_offset[HID_LAYOUT_REPORTS][5];
    bool output_other[HID_LAYOUT_REPORTS];
    unsigned reports, fields;
    bool has_ids;
    hid_field_t field[HID_LAYOUT_FIELDS];
} hid_layout_t;
typedef struct {
    bool keyboard, mouse;
    uint8_t keys[HID_KEYS_BYTES];
    uint8_t buttons;
    int32_t x, y, wheel, pan;
    unsigned report;
} hid_input_t;

/* Fail closed on unsupported / oversized descriptors rather than guessing. */
bool hid_layout_parse(hid_layout_t *out, const uint8_t *desc, size_t len);
bool hid_layout_decode(const hid_layout_t *layout, const uint8_t *data,
                       size_t len, hid_input_t *out);
size_t hid_layout_led_report(const hid_layout_t *layout, unsigned report, uint8_t leds, uint8_t data[HID_INPUT_BYTES]);
