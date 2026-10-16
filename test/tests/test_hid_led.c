#include "hid_input.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    // Three constant padding bits, then Num/Caps/Scroll, with report ID 7.
    uint8_t desc[]={0x05,1,0x09,6,0xa1,1,0x85,7,
        0x05,7,0x19,0,0x29,0x65,0x15,0,0x25,0x65,0x75,8,0x95,6,0x81,0,
        0x75,1,0x95,3,0x91,1,0x05,8,0x19,1,0x29,3,0x91,2,
        0x95,2,0x91,1,0xc0};
    hid_layout_t layout;uint8_t out[HID_INPUT_BYTES];
    assert(hid_layout_parse(&layout,desc,sizeof(desc)));
    assert(hid_layout_led_report(&layout,0,3,out)==2 && out[0]==7 && out[1]==24);
    assert(hid_layout_led_report(&layout,0,4,out)==2 && out[1]==32);
    assert(hid_layout_led_report(&layout,0,0,out)==2 && out[1]==0);
    assert(hid_layout_led_report(&layout,1,3,out)==0);
    layout.output_other[0]=true;assert(hid_layout_led_report(&layout,0,3,out)==0);
    puts("HID LED report IDs, bit offsets, padding and unrelated outputs passed");
}
