#pragma once
#include <stdbool.h>
#include <stdint.h>

#define KEYBOARD_PENDING_REPORTS 32

typedef struct {
    uint8_t reports[KEYBOARD_PENDING_REPORTS][8];
    unsigned head, count;
} keyboard_pending_t;

void keyboard_pending_clear(keyboard_pending_t *q);
bool keyboard_pending_push(keyboard_pending_t *q, const uint8_t report[8]);
const uint8_t *keyboard_pending_peek(const keyboard_pending_t *q);
void keyboard_pending_commit(keyboard_pending_t *q);
