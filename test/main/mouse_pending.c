#include "mouse_pending.h"
#include <limits.h>
#include <string.h>

static int16_t s16(const uint8_t *p) { return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1]<<8)); }
static int32_t clamp(int32_t value, int32_t low, int32_t high)
{ return value < low ? low : value > high ? high : value; }
static bool fits(int32_t previous, int32_t delta)
{ int64_t sum=(int64_t)previous+delta; return sum>=INT32_MIN && sum<=INT32_MAX; }
void mouse_pending_clear(mouse_pending_t *q) { memset(q,0,sizeof(*q)); }
bool mouse_pending_push(mouse_pending_t *q, const uint8_t report[7])
{
    mouse_segment_t next={.buttons=report[0],.x=s16(report+1),.y=s16(report+3),
                          .wheel=(int8_t)report[5],.pan=(int8_t)report[6]};
    if (q->count) {
        mouse_segment_t *last=&q->segments[(q->head+q->count-1)%MOUSE_PENDING_SEGMENTS];
        // Only coalesce while button state is identical: clicks and drags retain order.
        if (last->buttons==next.buttons && fits(last->x,next.x) && fits(last->y,next.y) &&
            fits(last->wheel,next.wheel) && fits(last->pan,next.pan)) {
            last->x+=next.x; last->y+=next.y; last->wheel+=next.wheel; last->pan+=next.pan;
            return true;
        }
    }
    if (q->count==MOUSE_PENDING_SEGMENTS) return false;
    q->segments[(q->head+q->count++)%MOUSE_PENDING_SEGMENTS]=next;
    return true;
}
bool mouse_pending_peek(const mouse_pending_t *q, uint8_t report[7])
{
    if (!q->count) return false;
    const mouse_segment_t *first=&q->segments[q->head];
    int16_t x=clamp(first->x,-32768,32767), y=clamp(first->y,-32768,32767);
    report[0]=first->buttons;
    report[1]=(uint16_t)x; report[2]=(uint16_t)x>>8;
    report[3]=(uint16_t)y; report[4]=(uint16_t)y>>8;
    report[5]=(uint8_t)(int8_t)clamp(first->wheel,-127,127);
    report[6]=(uint8_t)(int8_t)clamp(first->pan,-127,127);
    return true;
}
void mouse_pending_commit(mouse_pending_t *q, const uint8_t report[7])
{
    if (!q->count) return;
    mouse_segment_t *first=&q->segments[q->head];
    first->x-=s16(report+1); first->y-=s16(report+3);
    first->wheel-=(int8_t)report[5]; first->pan-=(int8_t)report[6];
    if (!first->x && !first->y && !first->wheel && !first->pan) {
        q->head=(q->head+1)%MOUSE_PENDING_SEGMENTS; --q->count;
    }
}
