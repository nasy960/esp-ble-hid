#include "usb_health.h"
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "usb/usb_host.h"

#define HEALTH_DEVICES 8
#define IDLE_US 5000000LL
#define FAST_IDLE_US 500000LL
#define RATE_WINDOW_US 1000000LL
#define CHECK_US 1000000LL
#define DEADLINE_US 500000LL
#define RESET_COOLDOWN_US 60000000LL
typedef struct {
    bool used, disabled;
    uint8_t iface, id, length, data[64];
    uint32_t generation, sent;
    unsigned errors;
    int64_t retry_at;
} led_job_t;
typedef struct {
    led_job_t leds[8], *pending_led;
    uint32_t led_generation;
    usb_device_handle_t device;
    usb_transfer_t *transfer;
    uint8_t address;
    bool gone, pending, held;
    uint32_t reports, window_reports, recent_reports;
    int64_t window_started, fast_until;
    unsigned failures;
    uint32_t probes, replies, stalls;
    int64_t last_input, next_probe, deadline;
} health_device_t;
static const char *TAG = "usb_health";
static usb_host_client_handle_t client;
static health_device_t devices[HEALTH_DEVICES];
static int64_t reset_allowed_at;

static void health_event(const usb_host_client_event_msg_t *event, void *arg)
{
    (void)arg;
    if (event->event != USB_HOST_CLIENT_EVENT_DEV_GONE) return;
    for (unsigned i=0; i<HEALTH_DEVICES; ++i)
        if (devices[i].device == event->dev_gone.dev_hdl) devices[i].gone = true;
}
static void failed(health_device_t *d, int status)
{
    if (d->failures < 3) ++d->failures;
    ESP_LOGW(TAG, "addr=%u probe failure=%u status=%d pending=%d",
             d->address, d->failures, status, d->pending);
}
static void probe_done(usb_transfer_t *transfer)
{
    health_device_t *d = transfer->context;
    d->pending = false;
    if (d->gone) return;
    if (d->pending_led) {
        led_job_t *job = d->pending_led;
        d->pending_led = NULL;
        if (transfer->status == USB_TRANSFER_STATUS_COMPLETED || transfer->status == USB_TRANSFER_STATUS_STALL)
            d->failures = 0;
        if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
            job->sent = d->led_generation;
            job->errors = 0;
        } else {
            ++job->errors;
            job->disabled = transfer->status == USB_TRANSFER_STATUS_STALL || job->errors >= 3;
            job->retry_at = esp_timer_get_time()+1000000;
            ESP_LOGW(TAG, "Keyboard LED output addr=%u iface=%u status=%d disabled=%d",
                     d->address, job->iface, transfer->status, job->disabled);
        }
        return;
    }
    // STALL is an explicit device response, not a silent bus.
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED ||
        transfer->status == USB_TRANSFER_STATUS_STALL) {
        ++d->replies;
        if (transfer->status == USB_TRANSFER_STATUS_STALL) ++d->stalls;
        if (d->failures || d->replies == 1)
            ESP_LOGI(TAG, "addr=%u control path responsive, status=%d; idle input alone will not reset USB",
                     d->address, transfer->status);
        d->failures = 0;
    } else if (transfer->status != USB_TRANSFER_STATUS_CANCELED &&
               transfer->status != USB_TRANSFER_STATUS_NO_DEVICE) {
        failed(d, transfer->status);
    }
}
esp_err_t usb_health_init(void)
{
    const usb_host_client_config_t config = {
        .is_synchronous = false, .max_num_event_msg = 16,
        .async = {.client_event_callback = health_event},
    };
    return usb_host_client_register(&config, &client);
}
void usb_health_watch(uint8_t address, int64_t last_input, uint32_t reports, bool held)
{
    if (!client) return;
    health_device_t *free_slot = NULL;
    for (unsigned i=0; i<HEALTH_DEVICES; ++i) {
        health_device_t *d = &devices[i];
        if (d->device && d->address == address) {
            int64_t now = esp_timer_get_time();
            d->held = held;
            if (now - d->window_started >= RATE_WINDOW_US) {
                d->recent_reports = d->window_reports;
                d->window_reports = 0;
                d->window_started = now;
            }
            // Counter wrap is valid; context removal can decrease the aggregate.
            uint32_t delta = reports - d->reports;
            if (delta < 0x80000000u) {
                d->window_reports += delta;
                if (d->window_reports >= 8 && delta)
                    d->fast_until = now + 3000000;
            }
            d->reports = reports;
            if (last_input > d->last_input) {
                d->last_input = last_input;
                d->failures = 0;
                d->next_probe = 0;
                // A real input callback is also evidence of liveness.
                if (d->pending) d->deadline = last_input + IDLE_US;
            }
            return;
        }
        if (!d->device) free_slot = d;
    }
    if (!free_slot) return;
    health_device_t *d = free_slot;
    usb_transfer_t *transfer = NULL;
    if (usb_host_transfer_alloc(USB_SETUP_PACKET_SIZE+64, 0, &transfer) != ESP_OK) return;
    usb_device_handle_t device;
    if (usb_host_device_open(client, address, &device) != ESP_OK) {
        usb_host_transfer_free(transfer);
        return;
    }
    *d = (health_device_t){.device=device, .transfer=transfer, .address=address,
                          .last_input=esp_timer_get_time(), .window_started=esp_timer_get_time(),
                          .reports=reports, .held=held};
}
void usb_health_leds(uint8_t address, uint8_t iface, uint8_t report_id, const uint8_t *data, size_t length)
{
    if (!length || length>64) return;
    for (unsigned i=0;i<HEALTH_DEVICES;++i) {
        health_device_t *d=&devices[i];
        if (!d->device || d->gone || d->address!=address) continue;
        led_job_t *job=NULL;
        for (unsigned j=0;j<8;++j) {
            if(d->leds[j].used && d->leds[j].iface==iface && d->leds[j].id==report_id) {
                job=&d->leds[j]; break;
            }
            if(!d->leds[j].used) job=&d->leds[j];
        }
        if(!job) return;
        if(!job->used || job->length!=length || memcmp(job->data,data,length)) {
            job->used=true;job->iface=iface;job->id=report_id;job->length=length;
            memcpy(job->data,data,length);++job->generation;
        }
        return;
    }
}
static bool send_led(health_device_t *d, int64_t now)
{
    for(unsigned j=0;j<8;++j) {
        led_job_t *job=&d->leds[j];
        if(!job->used || job->disabled || job->generation==job->sent || now<job->retry_at) continue;
        usb_transfer_t *t=d->transfer;
        usb_setup_packet_t *setup=(usb_setup_packet_t *)t->data_buffer;
        *setup=(usb_setup_packet_t){.bmRequestType=0x21,.bRequest=9,
            .wValue=(2u<<8)|job->id,.wIndex=job->iface,.wLength=job->length};
        memcpy(t->data_buffer+USB_SETUP_PACKET_SIZE,job->data,job->length);
        t->device_handle=d->device;t->bEndpointAddress=0;t->num_bytes=USB_SETUP_PACKET_SIZE+job->length;
        t->callback=probe_done;t->context=d;
        esp_err_t err=usb_host_transfer_submit_control(client,t);
        job->retry_at=now+1000000;
        if(err==ESP_OK) {
            d->pending=true;d->pending_led=job;d->led_generation=job->generation;d->deadline=now+DEADLINE_US;
        }
        return true;
    }
    return false;
}
bool usb_health_poll(void)
{
    if (!client) return false;
    usb_host_client_handle_events(client, 0);
    const int64_t now = esp_timer_get_time();
    bool pending = false;
    for (unsigned i=0; i<HEALTH_DEVICES; ++i) {
        health_device_t *d = &devices[i];
        if (!d->device) continue;
        if (d->gone && !d->pending) {
            // Keep the transfer/context alive until its completion is delivered.
            if (usb_host_device_close(client, d->device) == ESP_OK) {
                usb_host_transfer_free(d->transfer);
                memset(d, 0, sizeof(*d));
            }
            continue;
        }
        if (d->pending) {
            pending = true;
            if (!d->gone && now >= d->deadline) {
                // timeout_ms is not supported by this USB stack. Observe a
                // stuck request repeatedly without reusing/freeing its buffer.
                failed(d, ESP_ERR_TIMEOUT);
                d->deadline = now + CHECK_US;
            }
        }
        if (!d->gone && d->failures >= 3 && now >= reset_allowed_at) {
            reset_allowed_at = now + RESET_COOLDOWN_US;
            ESP_LOGW(TAG, "addr=%u control path failed repeatedly; requesting USB root restart", d->address);
            return true;
        }
    }
    if (pending) return false;
    for(unsigned i=0;i<HEALTH_DEVICES;++i)
        if(devices[i].device && !devices[i].gone && send_led(&devices[i],now)) return false;
    if (now < reset_allowed_at) return false;
    for (unsigned i=0; i<HEALTH_DEVICES; ++i) {
        health_device_t *d = &devices[i];
        if (!d->device || d->gone || now-d->last_input < ((d->held || now < d->fast_until) ? FAST_IDLE_US : IDLE_US) || now < d->next_probe) continue;
        usb_transfer_t *t = d->transfer;
        usb_setup_packet_t *setup = (usb_setup_packet_t *)t->data_buffer;
        USB_SETUP_PACKET_INIT_GET_STATUS(setup);
        t->device_handle = d->device;
        t->bEndpointAddress = 0;
        t->num_bytes = USB_SETUP_PACKET_SIZE + 2;
        t->callback = probe_done;
        t->context = d;
        d->next_probe = now + CHECK_US;
        esp_err_t err = usb_host_transfer_submit_control(client, t);
        if (err == ESP_OK) {
            ++d->probes;
            d->pending = true;
            d->deadline = now + DEADLINE_US;
        } else {
            // Local allocation/state failures do not prove device failure.
            ESP_LOGW(TAG, "addr=%u probe submit: %s (no automatic reset)", d->address, esp_err_to_name(err));
        }
        break; // At most one control request in flight across the monitor.
    }
    return false;
}
void usb_health_reset_started(void)
{
    reset_allowed_at = esp_timer_get_time() + RESET_COOLDOWN_US;
    // Port teardown delivers completion and DEV_GONE asynchronously. Do not
    // free pending transfers, close handles early, or issue more requests.
    for (unsigned i=0; i<HEALTH_DEVICES; ++i)
        if (devices[i].device) devices[i].gone = true;
}
void usb_health_status(void)
{
    for (unsigned i=0; i<HEALTH_DEVICES; ++i) {
        health_device_t *d = &devices[i];
        if (d->device)
            ESP_LOGI(TAG, "addr=%u probes=%lu replies=%lu stalls=%lu failures=%u pending=%d gone=%d held=%d recent_reports=%lu fast=%d",
                     d->address, (unsigned long)d->probes, (unsigned long)d->replies,
                     (unsigned long)d->stalls, d->failures, d->pending, d->gone, d->held,
                     (unsigned long)d->recent_reports, esp_timer_get_time() < d->fast_until);
    }
}
