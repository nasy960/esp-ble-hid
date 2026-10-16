#include "keyboard_pending.h"
#include <string.h>

void keyboard_pending_clear(keyboard_pending_t *q) { memset(q, 0, sizeof(*q)); }
bool keyboard_pending_push(keyboard_pending_t *q, const uint8_t report[8])
{
    if (q->count && !memcmp(q->reports[(q->head + q->count - 1) % KEYBOARD_PENDING_REPORTS], report, 8))
        return true;
    if (q->count == KEYBOARD_PENDING_REPORTS) return false;
    memcpy(q->reports[(q->head + q->count++) % KEYBOARD_PENDING_REPORTS], report, 8);
    return true;
}
const uint8_t *keyboard_pending_peek(const keyboard_pending_t *q)
{
    return q->count ? q->reports[q->head] : NULL;
}
void keyboard_pending_commit(keyboard_pending_t *q)
{
    if (!q->count) return;
    q->head = (q->head + 1) % KEYBOARD_PENDING_REPORTS;
    --q->count;
}
