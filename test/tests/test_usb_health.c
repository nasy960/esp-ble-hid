// Actual asynchronous monitor with virtual time and ownership-checked USB mocks.
#include <stdlib.h>
#include <stdio.h>
#include "../main/usb_health.c"
static int64_t clock_us;
static usb_transfer_t *flight;
static int allocations, frees, opens, closes, submits;
static bool submit_fail, close_fail;
int64_t esp_timer_get_time(void) { return clock_us; }
const char *esp_err_to_name(esp_err_t e) { (void)e; return "injected"; }
esp_err_t usb_host_client_register(const usb_host_client_config_t *c, usb_host_client_handle_t *h)
{ assert(!c->is_synchronous); *h=(void *)1; return ESP_OK; }
esp_err_t usb_host_client_handle_events(usb_host_client_handle_t h, unsigned t)
{ assert(h && t==0); return ESP_OK; }
esp_err_t usb_host_device_open(usb_host_client_handle_t c, uint8_t a, usb_device_handle_t *h)
{ assert(c); ++opens; *h=(void *)(uintptr_t)a; return ESP_OK; }
esp_err_t usb_host_device_close(usb_host_client_handle_t c, usb_device_handle_t h)
{ assert(c && h); assert(!flight || flight->device_handle!=h); if(close_fail) return ESP_FAIL; ++closes; return ESP_OK; }
esp_err_t usb_host_transfer_alloc(size_t n, int iso, usb_transfer_t **out)
{ assert(!iso); *out=calloc(1,sizeof(**out)); (*out)->data_buffer=calloc(1,n); ++allocations; return ESP_OK; }
esp_err_t usb_host_transfer_free(usb_transfer_t *t)
{ assert(t!=flight); free(t->data_buffer); free(t); ++frees; return ESP_OK; }
esp_err_t usb_host_transfer_submit_control(usb_host_client_handle_t c, usb_transfer_t *t)
{
    assert(c && !flight && t->bEndpointAddress==0);
    usb_setup_packet_t *s=(void *)t->data_buffer;
    if(s->bRequest==0) assert(s->bmRequestType==0x80 && s->wLength==2 && t->num_bytes==10);
    else assert(s->bmRequestType==0x21 && s->bRequest==9 && (s->wValue>>8)==2 && t->num_bytes==8+s->wLength);
    ++submits;
    if(submit_fail) return ESP_FAIL;
    flight=t; return ESP_OK;
}
static void complete(int status)
{ assert(flight); usb_transfer_t *t=flight; flight=NULL; t->status=status; t->callback(t); }
static health_device_t *watch(uint8_t addr)
{
    usb_health_watch(addr,clock_us,0,false);
    for(unsigned i=0;i<HEALTH_DEVICES;++i) if(devices[i].address==addr) return &devices[i];
    abort();
}
static void detach(health_device_t *d)
{
    usb_host_client_event_msg_t event={.event=USB_HOST_CLIENT_EVENT_DEV_GONE,.dev_gone={.dev_hdl=d->device}};
    health_event(&event,NULL);
}
int main(void)
{
    assert(usb_health_init()==ESP_OK);
    health_device_t *d=watch(2);
    clock_us=IDLE_US-1; assert(!usb_health_poll() && submits==0);
    ++clock_us; assert(!usb_health_poll() && flight);
    complete(USB_TRANSFER_STATUS_COMPLETED);
    assert(d->replies==1 && d->failures==0);
    // A normal idle device or explicit STALL must never cause a reset.
    for(int i=0;i<20;++i) {
        clock_us+=CHECK_US; assert(!usb_health_poll() && flight);
        complete(i%2 ? USB_TRANSFER_STATUS_STALL : USB_TRANSFER_STATUS_COMPLETED);
    }
    assert(d->stalls==10 && d->failures==0);
    // Three completed errors trigger recovery, with cooldown after it.
    for(int i=0;i<3;++i) {
        clock_us+=CHECK_US; assert(!usb_health_poll() && flight);
        complete(USB_TRANSFER_STATUS_ERROR);
    }
    assert(usb_health_poll());
    usb_health_reset_started();
    close_fail=true; assert(!usb_health_poll() && d->device);
    close_fail=false; assert(!usb_health_poll() && !d->device);
    d=watch(3); clock_us+=IDLE_US;
    assert(!usb_health_poll() && !flight); // Cooldown.
    clock_us+=RESET_COOLDOWN_US;
    assert(!usb_health_poll() && flight);
    int submitted=submits;
    // Missing completion: no buffer reuse; three spaced observations.
    clock_us+=DEADLINE_US; assert(!usb_health_poll() && d->failures==1);
    clock_us+=CHECK_US; assert(!usb_health_poll() && d->failures==2);
    // Live input cancels the failure evidence even if a probe is still pending.
    usb_health_watch(3,clock_us,1,false); assert(d->failures==0);
    assert(!usb_health_poll());
    clock_us+=IDLE_US; assert(!usb_health_poll() && d->failures==1);
    clock_us+=CHECK_US; assert(!usb_health_poll() && d->failures==2);
    clock_us+=CHECK_US; assert(usb_health_poll());
    assert(submits==submitted);
    usb_health_reset_started();
    int old_frees=frees;
    assert(!usb_health_poll() && frees==old_frees); // Await late completion.
    complete(USB_TRANSFER_STATUS_NO_DEVICE);
    assert(!usb_health_poll() && frees==old_frees+1);
    // Local submission errors alone never reboot a healthy/idle device.
    d=watch(4); submit_fail=true; clock_us+=RESET_COOLDOWN_US;
    for(int i=0;i<5;++i) { clock_us+=CHECK_US; assert(!usb_health_poll() && !flight && d->failures==0); }
    submit_fail=false;
    clock_us+=CHECK_US; assert(!usb_health_poll() && flight);
    detach(d); assert(!usb_health_poll() && d->device);
    complete(USB_TRANSFER_STATUS_CANCELED);
    assert(!usb_health_poll() && !d->device);
    d=watch(5);
    clock_us+=100000;
    usb_health_watch(5,clock_us,12,false); // Recent burst, then silence.
    clock_us+=FAST_IDLE_US-1; assert(!usb_health_poll() && !flight);
    ++clock_us; assert(!usb_health_poll() && flight);
    complete(USB_TRANSFER_STATUS_COMPLETED);
    assert(d->failures==0); // Stopping mouse movement is not a fault.
    clock_us+=4000000;
    usb_health_watch(5,clock_us,13,true); // One held key, even without a burst.
    clock_us+=FAST_IDLE_US; assert(!usb_health_poll() && flight);
    complete(USB_TRANSFER_STATUS_STALL);
    assert(d->held && d->failures==0); // A legitimate hold is never released by idleness.
    usb_health_watch(5,clock_us,14,false); // Release received, back to slow checking.
    clock_us+=FAST_IDLE_US; assert(!usb_health_poll() && !flight);
    detach(d); assert(!usb_health_poll());
    d=watch(6);uint8_t caps=2,num=1;
    usb_health_leds(6,0,0,&caps,1);assert(!usb_health_poll() && flight);
    assert(flight->data_buffer[8]==2);
    usb_health_leds(6,0,0,&num,1); // Queue changes cannot mutate in-flight buffer.
    assert(flight->data_buffer[8]==2);
    complete(USB_TRANSFER_STATUS_COMPLETED);
    clock_us+=1000000;assert(!usb_health_poll() && flight && flight->data_buffer[8]==1);
    complete(USB_TRANSFER_STATUS_COMPLETED);
    int before=submits;assert(!usb_health_poll() && submits==before);
    usb_health_leds(6,0,0,&caps,1);clock_us+=1000000;
    assert(!usb_health_poll() && flight);complete(USB_TRANSFER_STATUS_STALL);
    assert(d->failures==0); // Unsupported LED output must not trigger a root reset.
    detach(d);assert(!usb_health_poll());
    assert(allocations==frees && opens==closes);
    puts("USB health: idle, STALL, failures, deadlines, live input, cooldown and detach passed");
}
