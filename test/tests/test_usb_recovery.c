// Exercise the firmware's actual USB lifecycle with injected driver failures.
#include "../main/hello_world_main.c"

static int64_t now_us;
static int opens, starts, stops, closes, releases, removes, connected;
static int protocol_failures, descriptor_failures, start_failures, stop_failures;
static int power_offs, power_ons;
static bool power_off_fail;
static size_t raw_length;
static int forwarded;
void usb_health_status(void) {}
void usb_health_reset_started(void) {}
static bool detach_on_protocol;
static hid_host_device_config_t callback;
static const uint8_t descriptor[] = {
    0x05,1,0x09,6,0xa1,1,0x05,7,0x19,0,0x29,0x65,0x15,0,0x25,0x65,
    0x75,8,0x95,6,0x81,0,0xc0
};
int64_t esp_timer_get_time(void) { return now_us; }
const char *esp_err_to_name(esp_err_t e) { (void)e; return "injected"; }
esp_err_t hid_host_device_get_params(hid_host_device_handle_t d, hid_host_dev_params_t *p)
{ (void)d; *p=(hid_host_dev_params_t){.addr=1,.proto=HID_PROTOCOL_KEYBOARD,.sub_class=1}; return ESP_OK; }
esp_err_t hid_host_get_device_info(hid_host_device_handle_t d, hid_host_dev_info_t *p)
{ (void)d; memset(p,0,sizeof(*p)); return ESP_OK; }
esp_err_t hid_host_device_open(hid_host_device_handle_t d, const hid_host_device_config_t *c)
{ (void)d; ++opens; callback=*c; return ESP_OK; }
esp_err_t hid_host_device_close(hid_host_device_handle_t d)
{ (void)d; ++closes; return ESP_OK; }
esp_err_t hid_host_device_start(hid_host_device_handle_t d)
{ (void)d; ++starts; if(start_failures) { --start_failures; return ESP_FAIL; } return ESP_OK; }
esp_err_t hid_host_device_stop(hid_host_device_handle_t d)
{ (void)d; ++stops; if(stop_failures) { --stop_failures; return ESP_FAIL; } return ESP_OK; }
esp_err_t hid_class_request_set_protocol(hid_host_device_handle_t d, int p)
{
    assert(p==HID_REPORT_PROTOCOL_REPORT);
    if(detach_on_protocol) {
        detach_on_protocol=false;
        callback.callback(d,HID_HOST_INTERFACE_EVENT_DISCONNECTED,callback.callback_arg);
        return ESP_FAIL;
    }
    if(protocol_failures) { --protocol_failures; return ESP_ERR_TIMEOUT; }
    return ESP_OK;
}
esp_err_t hid_class_request_set_idle(hid_host_device_handle_t d, uint8_t duration, uint8_t id)
{ (void)d; assert(duration==25 && id==0); return ESP_OK; }
const uint8_t *hid_host_get_report_descriptor(hid_host_device_handle_t d, size_t *length)
{
    (void)d;
    if(descriptor_failures) { --descriptor_failures; return NULL; }
    *length=sizeof(descriptor); return descriptor;
}
esp_err_t hid_host_device_get_raw_input_report_data(hid_host_device_handle_t d, uint8_t *b, size_t cap, size_t *n)
{ (void)d; assert(cap >= raw_length); memset(b,0,raw_length); *n=raw_length; return ESP_OK; }
esp_err_t usb_host_lib_set_root_port_power(bool enable)
{ if(enable) ++power_ons; else { if(power_off_fail) return ESP_FAIL; ++power_offs; } return ESP_OK; }
void ble_switch_input(unsigned s, const hid_input_t *in) { (void)s; (void)in; ++forwarded; }
void ble_switch_usb_connected(unsigned s) { (void)s; ++connected; }
void ble_switch_release_input(unsigned s) { (void)s; ++releases; }
void ble_switch_remove(unsigned s) { (void)s; ++removes; }
static void tick(usb_input_context_t *ctx, int64_t delta)
{ now_us+=delta; service_device(ctx); }
static usb_input_context_t *attach(void)
{
    open_device((void *)(uintptr_t)1);
    usb_input_context_t *ctx=&inputs[0];
    assert(ctx->in_use);
    tick(ctx,249999); assert(ctx->stage==SETUP_OPEN);
    tick(ctx,1); assert(ctx->stage==SETUP_PROTOCOL);
    return ctx;
}
int main(void)
{
    protocol_failures=1; descriptor_failures=1; start_failures=1;
    usb_input_context_t *ctx=attach();
    tick(ctx,0); assert(ctx->stage==SETUP_PROTOCOL && ctx->failures==1);
    tick(ctx,249999); assert(ctx->failures==1);
    tick(ctx,1); assert(ctx->stage==SETUP_IDLE);
    tick(ctx,0); assert(ctx->stage==SETUP_DESCRIPTOR);
    tick(ctx,0); assert(ctx->stage==SETUP_DESCRIPTOR && ctx->failures==2);
    tick(ctx,250000); assert(ctx->stage==SETUP_START);
    tick(ctx,0); assert(ctx->stage==INPUT_STOP && releases==1);
    tick(ctx,250000); assert(ctx->stage==SETUP_START && stops==1);
    tick(ctx,0); assert(ctx->stage==INPUT_RUNNING && ctx->layout_ready);
    assert(opens==1 && starts==2 && closes==0);

    // Error releases cached keys and restarts through STOP, never closes a live handle.
    ctx->keys[0][0]=16;
    callback.callback(ctx->device,HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR,ctx);
    assert(releases==2 && !ctx->layout_ready && ctx->keys[0][0]==0);
    stop_failures=1;
    tick(ctx,0); assert(ctx->stage==INPUT_STOP);
    tick(ctx,100000); assert(ctx->stage==INPUT_STOP);
    tick(ctx,250000); assert(ctx->stage==SETUP_START);
    tick(ctx,0); assert(ctx->stage==INPUT_RUNNING && closes==0);

    // Disconnect callback retains the context until its owning task finalizes it.
    callback.callback(ctx->device,HID_HOST_INTERFACE_EVENT_DISCONNECTED,ctx);
    assert(ctx->in_use && !ctx->layout_ready && closes==0);
    tick(ctx,0); assert(!ctx->in_use && closes==1);
    ctx=attach();
    detach_on_protocol=true;
    tick(ctx,0); assert(ctx->in_use && ctx->disconnected && closes==1);
    int previous_starts=starts;
    tick(ctx,0); assert(!ctx->in_use && closes==2 && starts==previous_starts);

    // Enumeration recovery only runs with no working input, with a nonblocking off interval.
    no_input_since=now_us;
    now_us+=9999999; service_root_port(false); assert(power_offs==0);
    now_us+=1; service_root_port(false); assert(power_offs==1 && power_ons==0);
    now_us+=499999; service_root_port(false); assert(power_ons==0);
    now_us+=1; service_root_port(false); assert(power_ons==1);
    for(int i=0;i<20;++i) { now_us+=1000000; service_root_port(true); }
    assert(power_offs==1);
    service_root_port(false); assert(power_offs==1);

    ctx=attach();
    tick(ctx,0); tick(ctx,0); tick(ctx,0); tick(ctx,0);
    assert(ctx->stage==INPUT_RUNNING && ctx->layout_ready);
    raw_length=6;
    callback.callback(ctx->device,HID_HOST_INTERFACE_EVENT_INPUT_REPORT,ctx);
    assert(ctx->stats.callbacks==1 && ctx->stats.decoded==1 && forwarded==1);
    raw_length=1;
    callback.callback(ctx->device,HID_HOST_INTERFACE_EVENT_INPUT_REPORT,ctx);
    assert(ctx->stats.callbacks==2 && ctx->stats.decode_errors==1 && forwarded==1);
    raw_length=0;
    callback.callback(ctx->device,HID_HOST_INTERFACE_EVENT_INPUT_REPORT,ctx);
    assert(ctx->stats.read_errors==1 && ctx->stats.callbacks==3);
    assert(!usb_console_command("status"));
    assert(usb_console_command("usb status"));
    service_usb_commands();
    assert(power_offs==1 && ctx->layout_ready);
    power_off_fail=true;
    assert(usb_console_command("usb reset"));
    service_usb_commands();
    assert(ctx->layout_ready && !ctx->reset_wait && power_on_at==0);
    power_off_fail=false;
    assert(usb_console_command("usb reset"));
    service_usb_commands();
    assert(power_offs==2 && ctx->reset_wait && !ctx->layout_ready);
    previous_starts=starts;
    raw_length=6;
    callback.callback(ctx->device,HID_HOST_INTERFACE_EVENT_INPUT_REPORT,ctx);
    assert(forwarded==1 && ctx->stats.callbacks==4);
    tick(ctx,0); assert(starts==previous_starts);
    assert(usb_console_command("usb reset"));
    service_usb_commands(); assert(power_offs==2); // Coalesce while restarting.
    now_us+=499999; service_root_port(false); assert(power_ons==1);
    now_us+=1; service_root_port(false); assert(power_ons==2);
    tick(ctx,0); assert(starts==previous_starts); // Still awaiting disconnect.
    callback.callback(ctx->device,HID_HOST_INTERFACE_EVENT_DISCONNECTED,ctx);
    tick(ctx,0); assert(!ctx->in_use);
    ctx=attach(); assert(!ctx->reset_wait && ctx->stats.callbacks==0);
    puts("USB setup, transfer recovery, diagnostics and manual root-reset tests passed");
}
