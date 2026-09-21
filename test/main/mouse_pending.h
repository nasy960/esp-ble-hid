#pragma once
#include <stdbool.h>
#include <stdint.h>
#define MOUSE_PENDING_SEGMENTS 32

typedef struct {
    uint8_t buttons;
    int32_t x, y, wheel, pan;
} mouse_segment_t;
typedef struct {
    mouse_segment_t segments[MOUSE_PENDING_SEGMENTS];
    unsigned head, count;
} mouse_pending_t;
void mouse_pending_clear(mouse_pending_t *q);
bool mouse_pending_push(mouse_pending_t *q, const uint8_t report[7]);
bool mouse_pending_peek(const mouse_pending_t *q, uint8_t report[7]);
/* Commit only after BLE accepted the exact report returned by peek. */
void mouse_pending_commit(mouse_pending_t *q, const uint8_t report[7]);
