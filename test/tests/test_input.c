#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "hid_input.h"
#include "input_router.h"
#include "ble_report_map.h"
#include "status_pattern.h"
#include "mouse_pending.h"

// Captured from the user's 05AC:024F receiver: 5 keys plus vendor byte.
static const uint8_t keyboard[] = {
0x05,0x01,0x09,0x06,0xa1,0x01,0x05,0x07,0x19,0xe0,0x29,0xe7,0x15,0x00,0x25,0x01,
0x75,0x01,0x95,0x08,0x81,0x02,0x95,0x01,0x75,0x08,0x81,0x01,0x95,0x05,0x75,0x01,
0x05,0x08,0x19,0x01,0x29,0x05,0x91,0x02,0x95,0x01,0x75,0x03,0x91,0x01,0x05,0x07,
0x19,0x00,0x2a,0xff,0x00,0x95,0x05,0x75,0x08,0x15,0x00,0x26,0xff,0x00,0x81,0x00,
0x05,0xff,0x09,0x03,0x75,0x08,0x95,0x01,0x81,0x02,0x05,0x0c,0x09,0x00,0x15,0x80,
0x25,0x7f,0x95,0x40,0x75,0x08,0xb1,0x02,0xc0};
// Captured from the user's 046D:C548 receiver: 16-byte NKRO bitmap.
static const uint8_t nkro[] = {
0x05,0x01,0x09,0x06,0xa1,0x01,0x05,0x07,0x19,0xe0,0x29,0xe7,0x15,0x00,0x25,0x01,
0x75,0x01,0x95,0x08,0x81,0x02,0x95,0x05,0x05,0x08,0x19,0x01,0x29,0x05,0x91,0x02,
0x95,0x01,0x75,0x03,0x91,0x03,0x95,0x70,0x75,0x01,0x05,0x07,0x19,0x04,0x29,0x73,
0x81,0x02,0x95,0x05,0x19,0x87,0x29,0x8b,0x81,0x02,0x95,0x03,0x19,0x90,0x29,0x92,
0x81,0x02,0xc0};
// Synthetic mouse: report ID 3, 8 buttons, signed packed 12-bit X/Y and two wheels.
static const uint8_t mouse[] = {
0x05,1,0x09,2,0xa1,1,0x85,3,0x05,9,0x19,1,0x29,8,0x15,0,0x25,1,
0x75,1,0x95,8,0x81,2,0x05,1,0x09,0x30,0x09,0x31,0x16,0,0xf8,0x26,0xff,7,
0x75,12,0x95,2,0x81,6,0x09,0x38,0x15,0x81,0x25,0x7f,0x75,8,0x95,1,0x81,6,
0x05,0x0c,0x0a,0x38,2,0x81,6,0xc0};
static bool pressed(const hid_input_t *in, unsigned k) { return (in->keys[k/8] >> (k%8)) & 1; }
static void put_key(hid_input_t *in, unsigned k) { in->keyboard=true; in->keys[k/8] |= 1u<<(k%8); }
static void parser_tests(void)
{
    hid_layout_t layout;
    hid_input_t in;
    assert(hid_layout_parse(&layout, keyboard, sizeof(keyboard)));
    uint8_t report[] = {2,0,4,9,0x0d,0x0e,0x1a,0x55};
    assert(hid_layout_decode(&layout, report, sizeof(report), &in));
    assert(in.keyboard && !in.mouse && pressed(&in,0xe1) && pressed(&in,4) && pressed(&in,0x1a));
    assert(!pressed(&in,0x55)); // Vendor byte must not become a sixth key.
    assert(!hid_layout_decode(&layout,report,7,&in));
    report[2]=1; assert(!hid_layout_decode(&layout,report,8,&in));
    memset(report,0,sizeof(report)); assert(hid_layout_decode(&layout,report,8,&in));
    for (unsigned i=0;i<32;++i) assert(in.keys[i]==0);
    assert(hid_layout_parse(&layout,nkro,sizeof(nkro)));
    uint8_t bitmap[16]={1,1}; bitmap[14]=0x80; bitmap[15]=0x81;
    assert(hid_layout_decode(&layout,bitmap,sizeof(bitmap),&in));
    assert(pressed(&in,0xe0) && pressed(&in,4) && pressed(&in,0x73) && pressed(&in,0x87) && pressed(&in,0x92));
    assert(hid_layout_parse(&layout,mouse,sizeof(mouse)));
    uint8_t m[]={3,0x18,0xff,0x2f,0x01,0xff,2}; // X=-1,Y=18,wheel=-1,pan=2
    assert(hid_layout_decode(&layout,m,sizeof(m),&in));
    assert(in.mouse && !in.keyboard && in.buttons==0x18 && in.x==-1 && in.y==18 && in.wheel==-1 && in.pan==2);
    m[0]=2; assert(!hid_layout_decode(&layout,m,sizeof(m),&in));
    for(size_t n=0;n<sizeof(mouse);++n) assert(!hid_layout_parse(&layout,mouse,n));
    uint8_t bad[]={0x05,7,0x75,0x20,0x96,0xff,0xff,0x81,0};
    assert(!hid_layout_parse(&layout,bad,sizeof(bad)));
    // Check the actual BLE Report Map against the reports sent on GATT.
    assert(hid_layout_parse(&layout,report_map,sizeof(report_map)));
    uint8_t ble_key[]={1,2,0,4,5,6,7,8,9};
    assert(hid_layout_decode(&layout,ble_key,sizeof(ble_key),&in));
    assert(in.keyboard && pressed(&in,0xe1) && pressed(&in,4) && pressed(&in,9));
    uint8_t ble_mouse[]={2,0x10,0x0c,0xfe,0xf4,1,0xff,2};
    assert(hid_layout_decode(&layout,ble_mouse,sizeof(ble_mouse),&in));
    assert(in.mouse && in.buttons==16 && in.x==-500 && in.y==500 && in.wheel==-1 && in.pan==2);
    // Deterministic malformed-descriptor fuzzing under ASan/UBSan.
    uint32_t random=12345; uint8_t bytes[128];
    for (unsigned i=0;i<10000;++i) {
        for(unsigned j=0;j<sizeof(bytes);++j) {random=random*1664525u+1013904223u; bytes[j]=random>>24;}
        if(hid_layout_parse(&layout,bytes,i%sizeof(bytes))) hid_layout_decode(&layout,bytes,sizeof(bytes),&in);
    }
}
static void router_tests(void)
{
    input_router_t r; input_router_init(&r,4);
    router_output_t o; hid_input_t k={0},m={.mouse=true}; put_key(&k,4);
    input_router_apply(&r,0,&k,false,true,&o); assert(o.keyboard[2]==4 && r.target==0);
    m.buttons=8; input_router_apply(&r,1,&m,false,true,&o);
    assert(o.switched && r.target==1 && o.keyboard[2]==0 && o.mouse[0]==0);
    input_router_apply(&r,1,&m,false,true,&o); assert(!o.switched); // Held side button switches only once.
    m.buttons=0; input_router_apply(&r,1,&m,false,true,&o); assert(o.keyboard[2]==0);
    memset(k.keys,0,32); input_router_apply(&r,0,&k,false,true,&o);
    put_key(&k,4); input_router_apply(&r,0,&k,false,true,&o); assert(o.keyboard[2]==4);
    m.buttons=16; m.x=-500; m.wheel=1; input_router_apply(&r,1,&m,false,true,&o);
    assert(!o.switched && o.mouse[0]==16 && o.mouse[1]==0x0c && o.mouse[2]==0xfe && o.mouse[5]==1);
    m.buttons=8; input_router_apply(&r,1,&m,false,false,&o); assert(!o.switched && o.switch_requested);
    input_router_apply(&r,1,&m,false,true,&o); assert(!o.switched);
    m.buttons=0; input_router_apply(&r,1,&m,false,true,&o);
    m.buttons=8; input_router_apply(&r,1,&m,false,true,&o); assert(o.switched && r.target==0);
    memset(&k,0,sizeof(k)); put_key(&k,0xe0); put_key(&k,0xe2); put_key(&k,0x45);
    input_router_apply(&r,0,&k,false,true,&o); assert(o.switched && !o.keyboard[0] && !o.keyboard[2]);
    input_router_apply(&r,0,&k,false,true,&o); assert(!o.switched);
    input_router_init(&r,4); memset(&k,0,sizeof(k)); put_key(&k,4);
    input_router_apply(&r,0,&k,false,true,&o); input_router_apply(&r,2,&k,false,true,&o);
    input_router_apply(&r,0,&k,true,true,&o); assert(o.keyboard[2]==4);
    input_router_apply(&r,2,&k,true,true,&o); assert(o.keyboard[2]==0);
    for(unsigned i=4;i<11;++i) put_key(&k,i);
    input_router_apply(&r,0,&k,false,true,&o);
    for(unsigned i=2;i<8;++i) assert(o.keyboard[i]==1);
    input_router_init(&r,5); m.buttons=8;
    input_router_apply(&r,1,&m,false,true,&o); assert(!o.switched && o.mouse[0]==8);
    m.buttons=16; input_router_apply(&r,1,&m,false,true,&o); assert(o.switched && o.mouse[0]==0);
}
static void pairing_tests(void)
{
    input_router_t r; input_router_init(&r,4);
    router_output_t o; hid_input_t k={0}, m={.mouse=true,.buttons=8};
    put_key(&k,0xe0); put_key(&k,0xe2); put_key(&k,0xe1); put_key(&k,0x50);
    input_router_apply(&r,0,&k,false,false,&o);
    assert(o.pairing_action==ROUTER_PAIR_A && !o.switched && r.target==0);
    for(unsigned i=0;i<8;++i) assert(o.keyboard[i]==0);
    // Repeat packets while held must not restart the 120-second enrollment timer.
    input_router_apply(&r,0,&k,false,false,&o); assert(o.pairing_action==ROUTER_PAIR_NONE);
    input_router_apply(&r,1,&m,false,true,&o); assert(!o.switched && !o.switch_requested);
    // Release modifiers first: never leak the still-held arrow into the PC.
    memset(k.keys,0,32); put_key(&k,0x50);
    input_router_apply(&r,0,&k,false,true,&o); assert(!o.keyboard[2]);
    put_key(&k,0xe0); put_key(&k,0xe1); put_key(&k,0xe2);
    input_router_apply(&r,0,&k,false,true,&o); assert(o.pairing_action==ROUTER_PAIR_NONE);
    input_router_apply(&r,0,&k,true,true,&o);
    // Right-side modifiers are valid too, and no BLE peer needs to be ready.
    memset(k.keys,0,32); put_key(&k,0xe4); put_key(&k,0xe5); put_key(&k,0xe6); put_key(&k,0x4f);
    input_router_apply(&r,0,&k,false,false,&o); assert(o.pairing_action==ROUTER_PAIR_B);
    input_router_apply(&r,0,&k,true,false,&o);
    memset(k.keys,0,32); put_key(&k,0xe0); put_key(&k,0xe1); put_key(&k,0xe2); put_key(&k,0x51);
    input_router_apply(&r,0,&k,false,false,&o); assert(o.pairing_action==ROUTER_PAIR_CANCEL);
    input_router_apply(&r,0,&k,true,false,&o);
    // Ambiguous left+right does not enroll either slot; release before retrying.
    memset(k.keys,0,32); put_key(&k,0xe0); put_key(&k,0xe1); put_key(&k,0xe2); put_key(&k,0x50); put_key(&k,0x4f);
    input_router_apply(&r,0,&k,false,false,&o); assert(o.pairing_action==ROUTER_PAIR_NONE && !o.keyboard[2]);
    k.keys[0x4f/8] &= ~(1u<<(0x4f%8));
    input_router_apply(&r,0,&k,false,false,&o); assert(o.pairing_action==ROUTER_PAIR_NONE);
    input_router_apply(&r,0,&k,true,false,&o);
    // Ordinary arrow input (without all three modifiers) is still forwarded.
    memset(k.keys,0,32); put_key(&k,0x50);
    input_router_apply(&r,0,&k,false,true,&o); assert(o.pairing_action==ROUTER_PAIR_NONE && o.keyboard[2]==0x50);
}
static void reset_shortcut_tests(void)
{
    input_router_t r; input_router_init(&r,4);
    router_output_t o; hid_input_t k={0};
    put_key(&k,0xe0); put_key(&k,0xe1); put_key(&k,0xe2); put_key(&k,0x2a);
    input_router_apply(&r,0,&k,false,false,&o);
    for (unsigned i=0;i<8;++i) assert(o.keyboard[i]==0);
    assert(!input_router_reset_due(&r,100));
    assert(!input_router_reset_due(&r,3099));
    assert(input_router_reset_due(&r,3100));
    assert(!input_router_reset_due(&r,9000)); // One shot even without more reports.
    input_router_apply(&r,0,&k,false,false,&o);
    assert(!input_router_reset_due(&r,10000));
    input_router_apply(&r,0,&k,true,false,&o);
    assert(!input_router_reset_due(&r,11000));
    input_router_apply(&r,0,&k,false,false,&o);
    assert(!input_router_reset_due(&r,12000));
    // Unplug before the deadline cancels the pending reset.
    input_router_apply(&r,0,&k,true,false,&o);
    assert(!input_router_reset_due(&r,15000));
    input_router_apply(&r,0,&k,false,false,&o);
    assert(!input_router_reset_due(&r,16000));
    // Releasing just a modifier also cancels; holding again restarts the timer.
    k.keys[0xe0/8] &= ~(1u<<(0xe0%8));
    input_router_apply(&r,0,&k,false,false,&o);
    assert(!input_router_reset_due(&r,19000));
    put_key(&k,0xe0);
    input_router_apply(&r,0,&k,false,false,&o);
    assert(!input_router_reset_due(&r,20000));
    assert(input_router_reset_due(&r,23000));
    // Registration/reset combinations cannot erase or enroll simultaneously.
    put_key(&k,0x50);
    input_router_apply(&r,0,&k,false,true,&o);
    assert(o.pairing_action==ROUTER_PAIR_NONE && !o.switched);
    assert(!input_router_reset_due(&r,26000));
    input_router_apply(&r,0,&k,true,false,&o);
    memset(k.keys,0,sizeof(k.keys)); put_key(&k,0x2a);
    input_router_apply(&r,0,&k,false,false,&o);
    assert(o.keyboard[2]==0x2a && !r.reset_held); // Ordinary Backspace works.
}
static void led_tests(void)
{
    uint8_t rgb[3];
    status_pattern_t p=status_pattern_select(0,true,-1,false);
    assert(p.mode==STATUS_READY && p.slot==0);
    status_pattern_rgb(p,0,8,rgb); assert(rgb[0]==0 && rgb[2]==0 && rgb[1]==8);
    status_pattern_rgb(p,500,8,rgb); assert(!(rgb[0]|rgb[2]|rgb[1]));
    status_pattern_rgb(p,1000,8,rgb); assert(rgb[1]==8);
    // Pairing B overrides selected A and no USB devices; retain B's blue color.
    p=status_pattern_select(0,true,1,true); assert(p.mode==STATUS_PAIRING && p.slot==1);
    status_pattern_rgb(p,0,8,rgb); assert(rgb[2]==8 && rgb[0]==0 && rgb[1]==0);
    status_pattern_rgb(p,125,8,rgb); assert(!(rgb[0]|rgb[2]|rgb[1]));
    status_pattern_rgb(p,250,8,rgb); assert(rgb[2]==8);
    // No USB inputs is red even with a BLE PC ready. A hub alone is not an input.
    p=status_pattern_select(1,true,-1,true); assert(p.mode==STATUS_NONE);
    status_pattern_rgb(p,9999,8,rgb); assert(rgb[0]==8 && rgb[2]==0 && rgb[1]==0);
    p=status_pattern_select(1,false,-1,false); assert(p.mode==STATUS_WAITING);
    status_pattern_rgb(p,0,8,rgb); assert(rgb[2]==8);
    status_pattern_rgb(p,100,8,rgb); assert(!(rgb[0]|rgb[2]|rgb[1]));
    status_pattern_rgb(p,2000,8,rgb); assert(rgb[2]==8);
    status_pattern_rgb(p,0,0,rgb); assert(!(rgb[0]|rgb[2]|rgb[1]));
}
static int16_t report_s16(const uint8_t *p) { return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1]<<8)); }
static void mouse_pending_tests(void)
{
    mouse_pending_t q; mouse_pending_clear(&q);
    uint8_t report[7], again[7];
    assert(!mouse_pending_peek(&q,report));
    uint8_t move[]={0,0x30,0x75,0xd0,0x8a,100,(uint8_t)-100}; // 30000,-30000
    assert(mouse_pending_push(&q,move)); assert(mouse_pending_push(&q,move));
    assert(q.count==1);
    assert(mouse_pending_peek(&q,report));
    // Failed notification must leave exactly the same data available for retry.
    assert(mouse_pending_peek(&q,again) && !memcmp(report,again,7));
    int32_t x=0,y=0,wheel=0,pan=0; unsigned packets=0;
    while(mouse_pending_peek(&q,report)) {
        x+=report_s16(report+1); y+=report_s16(report+3);
        wheel+=(int8_t)report[5]; pan+=(int8_t)report[6];
        mouse_pending_commit(&q,report); ++packets;
    }
    assert(packets==2 && x==60000 && y==-60000 && wheel==200 && pan==-200);
    uint8_t down[]={1,2,0,3,0,0,0}, up[]={0,4,0,5,0,0,0};
    assert(mouse_pending_push(&q,down)); assert(mouse_pending_push(&q,up));
    assert(q.count==2); assert(mouse_pending_peek(&q,report) && report[0]==1 && report_s16(report+1)==2);
    mouse_pending_commit(&q,report);
    assert(mouse_pending_peek(&q,report) && report[0]==0 && report_s16(report+1)==4);
    mouse_pending_commit(&q,report);
    // Button-only reports and ring wrap preserve presses/releases as well.
    for(unsigned cycle=0;cycle<3;++cycle) {
        for(unsigned i=0;i<MOUSE_PENDING_SEGMENTS;++i) {
            uint8_t edge[7]={i&1}; assert(mouse_pending_push(&q,edge));
        }
        uint8_t edge[7]={0}; assert(!mouse_pending_push(&q,edge));
        for(unsigned i=0;i<MOUSE_PENDING_SEGMENTS;++i) {
            assert(mouse_pending_peek(&q,report) && report[0]==(i&1));
            mouse_pending_commit(&q,report);
        }
    }
    assert(mouse_pending_push(&q,down));
    mouse_pending_clear(&q); // Switching/disconnect must not replay old movement.
    assert(!mouse_pending_peek(&q,report));
}
int main(void) { parser_tests(); router_tests(); pairing_tests(); reset_shortcut_tests(); led_tests(); mouse_pending_tests(); puts("HID, routing, pairing/reset, LED and mouse retry tests passed"); }
