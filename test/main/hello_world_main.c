/* SPDX-License-Identifier: CC0-1.0 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "ble_switch.h"
#include "input_router.h"
#include "status_led.h"
#include "usb_health.h"

static const char *TAG = "usb_input";
static QueueHandle_t connection_queue;
static portMUX_TYPE source_lock = portMUX_INITIALIZER_UNLOCKED;
enum { USB_COMMAND_STATUS = 1, USB_COMMAND_RESET = 2, USB_COMMAND_AUTO_RESET = 4 };
static unsigned usb_commands;
typedef struct {
    uint32_t callbacks, decoded, read_errors, decode_errors, transfer_errors;
    int64_t last_callback;
    bool held;
} usb_input_stats_t;
typedef struct {
    bool in_use, layout_ready, disconnected, recover, reset_wait;
    enum { SETUP_OPEN, SETUP_PROTOCOL, SETUP_IDLE, SETUP_DESCRIPTOR,
           SETUP_START, INPUT_RUNNING, INPUT_STOP, INPUT_CLOSE } stage;
    unsigned source, failures;
    int64_t retry_at;
    usb_input_stats_t stats;
    hid_host_device_handle_t device;
    hid_host_dev_params_t params;
    hid_layout_t layout;
    uint8_t keys[HID_LAYOUT_REPORTS][HID_KEYS_BYTES];
    uint8_t buttons[HID_LAYOUT_REPORTS];
} usb_input_context_t;
static usb_input_context_t inputs[ROUTER_SOURCES];

static const char *protocol_name(hid_protocol_t protocol)
{
    switch (protocol) {
    case HID_PROTOCOL_KEYBOARD: return "KEYBOARD";
    case HID_PROTOCOL_MOUSE: return "MOUSE";
    default: return "HID";
    }
}
// Only app_main reuses contexts or closes handles. A disconnect callback must
// not free/reuse a source while synchronous setup is still using it.
static void release_context(usb_input_context_t *ctx)
{
    ble_switch_remove(ctx->source);
    ctx->in_use = false;
}
static usb_input_context_t *allocate_context(void)
{
    for (unsigned i = 0; i < ROUTER_SOURCES; ++i) if (!inputs[i].in_use) {
        memset(&inputs[i], 0, sizeof(inputs[i]));
        inputs[i].in_use = true;
        inputs[i].source = i;
        return &inputs[i];
    }
    return NULL;
}
static void interface_callback(hid_host_device_handle_t device,
                               hid_host_interface_event_t event, void *arg)
{
    usb_input_context_t *ctx = arg;
    hid_host_dev_params_t params = ctx->params;
    esp_err_t err;
    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT: {
        portENTER_CRITICAL(&source_lock);
        ++ctx->stats.callbacks;
        ctx->stats.last_callback = esp_timer_get_time();
        bool accepting = ctx->layout_ready && !ctx->disconnected && !ctx->recover;
        portEXIT_CRITICAL(&source_lock);
        if (!accepting) break;
        uint8_t data[HID_INPUT_BYTES]; size_t length = 0;
        err = hid_host_device_get_raw_input_report_data(device, data, sizeof(data), &length);
        if (err != ESP_OK || !length) {
            portENTER_CRITICAL(&source_lock);
            ++ctx->stats.read_errors;
            portEXIT_CRITICAL(&source_lock);
            break;
        }
        hid_input_t input;
        if (!hid_layout_decode(&ctx->layout, data, length, &input)) {
            portENTER_CRITICAL(&source_lock);
            ++ctx->stats.decode_errors;
            portEXIT_CRITICAL(&source_lock);
            ESP_LOGD(TAG, "Unparsed addr=%u iface=%u report (%u bytes)", params.addr, params.iface_num, (unsigned)length);
            break;
        }
        unsigned r = input.report;
        if (input.keyboard) memcpy(ctx->keys[r], input.keys, HID_KEYS_BYTES);
        if (input.mouse) ctx->buttons[r] = input.buttons;
        memset(input.keys, 0, sizeof(input.keys)); input.buttons = 0;
        for (unsigned i = 0; i < ctx->layout.reports; ++i) {
            for (unsigned k = 0; k < HID_KEYS_BYTES; ++k) input.keys[k] |= ctx->keys[i][k];
            input.buttons |= ctx->buttons[i];
        }
        // A manual root-port reset may have gated this source during decoding.
        // Serialize the final enqueue with that gate and its release event.
        portENTER_CRITICAL(&source_lock);
        if (ctx->layout_ready && !ctx->disconnected && !ctx->recover) {
            ++ctx->stats.decoded;
            ctx->stats.held = input.buttons != 0;
            for (unsigned k=0; k<HID_KEYS_BYTES; ++k)
                ctx->stats.held |= input.keys[k] != 0;
            ble_switch_input(ctx->source, &input);
        }
        portEXIT_CRITICAL(&source_lock);
#if CONFIG_INPUT_SWITCH_LOG_INPUT
        if (input.keyboard) {
            ESP_LOGI(TAG, "KEYBOARD addr=%u iface=%u raw (%u bytes)", params.addr, params.iface_num, (unsigned)length);
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, length, ESP_LOG_INFO);
        }
        if (input.mouse) {
            ESP_LOGI(TAG, "MOUSE addr=%u iface=%u buttons=0x%02X dx=%ld dy=%ld wheel=%ld pan=%ld",
                     params.addr, params.iface_num, input.buttons,
                     (long)input.x, (long)input.y, (long)input.wheel, (long)input.pan);
        }
#endif
        break;
    }
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "%s addr=%u iface=%u disconnected", protocol_name(params.proto), params.addr, params.iface_num);
        portENTER_CRITICAL(&source_lock);
        ctx->disconnected = true;
        ctx->layout_ready = false;
        portEXIT_CRITICAL(&source_lock);
        ble_switch_remove(ctx->source);
        break;
    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        portENTER_CRITICAL(&source_lock);
        ++ctx->stats.transfer_errors;
        ctx->recover = true;
        ctx->layout_ready = false;
        portEXIT_CRITICAL(&source_lock);
        ble_switch_release_input(ctx->source);
        memset(ctx->keys, 0, sizeof(ctx->keys)); memset(ctx->buttons, 0, sizeof(ctx->buttons));
        ESP_LOGW(TAG, "%s addr=%u iface=%u transfer error; released input, scheduling restart", protocol_name(params.proto), params.addr, params.iface_num);
        break;
    default: break;
    }
}
static void device_callback(hid_host_device_handle_t device, hid_host_driver_event_t event, void *arg)
{
    if (event == HID_HOST_DRIVER_EVENT_CONNECTED && xQueueSend(connection_queue, &device, 0) != pdTRUE)
        ESP_LOGE(TAG, "Connection queue full; reconnect USB device");
}
static void open_device(hid_host_device_handle_t device)
{
    hid_host_dev_params_t params;
    esp_err_t err = hid_host_device_get_params(device, &params);
    if (err != ESP_OK) return;
    ESP_LOGI(TAG, "Detected %s addr=%u iface=%u subclass=%u protocol=%u",
             protocol_name(params.proto), params.addr, params.iface_num, params.sub_class, params.proto);
    hid_host_dev_info_t info;
    if (hid_host_get_device_info(device, &info) == ESP_OK)
        ESP_LOGI(TAG, "addr=%u iface=%u VID=%04X PID=%04X", params.addr, params.iface_num, info.VID, info.PID);
    if (params.proto != HID_PROTOCOL_KEYBOARD && params.proto != HID_PROTOCOL_MOUSE) {
        ESP_LOGI(TAG, "Skipping extra HID addr=%u iface=%u (keyboard/mouse only)", params.addr, params.iface_num);
        return;
    }
    usb_input_context_t *ctx = allocate_context();
    if (!ctx) { ESP_LOGW(TAG, "Too many USB input interfaces"); return; }
    ctx->device = device;
    ctx->params = params;
    // Let receivers behind a newly powered hub settle before class requests.
    ctx->retry_at = esp_timer_get_time() + 250000;
}
static void service_device(usb_input_context_t *ctx)
{
    if (!ctx->in_use) return;
    portENTER_CRITICAL(&source_lock);
    bool disconnected = ctx->disconnected;
    bool recover = ctx->recover;
    ctx->recover = false;
    portEXIT_CRITICAL(&source_lock);
    if (disconnected) {
        esp_err_t err = hid_host_device_close(ctx->device);
        if (err == ESP_OK || err == ESP_ERR_INVALID_ARG) release_context(ctx);
        return;
    }
    if (ctx->reset_wait) return; // Wait for root-reset disconnect, never restart an old handle.
    if (recover) { ctx->stage = INPUT_STOP; ctx->retry_at = esp_timer_get_time() + 100000; }
    if (esp_timer_get_time() < ctx->retry_at) return;
    esp_err_t err = ESP_OK;
    switch (ctx->stage) {
    case SETUP_OPEN: {
        const hid_host_device_config_t config = {.callback=interface_callback, .callback_arg=ctx};
        err = hid_host_device_open(ctx->device, &config);
        if (err == ESP_OK) { ctx->stage = SETUP_PROTOCOL; }
        // An unopened interface can disappear without an interface callback.
        else if (err == ESP_ERR_INVALID_ARG) { release_context(ctx); return; }
        break;
    }
    case SETUP_PROTOCOL:
        if (ctx->params.sub_class == HID_SUBCLASS_BOOT_INTERFACE)
            err = hid_class_request_set_protocol(ctx->device, HID_REPORT_PROTOCOL_REPORT);
        if (err == ESP_OK) ctx->stage = SETUP_IDLE;
        break;
    case SETUP_IDLE:
        if (ctx->params.proto == HID_PROTOCOL_KEYBOARD) {
            // 25 x 4 ms: a periodic current-state report repairs a lost release
            // on receivers that support SET_IDLE. Do not time out genuine holds
            // on receivers that ignore it.
            esp_err_t idle_err = hid_class_request_set_idle(ctx->device, 25, 0);
            if (idle_err != ESP_OK) ESP_LOGW(TAG, "SET_IDLE: %s", esp_err_to_name(idle_err));
        }
        ctx->stage = SETUP_DESCRIPTOR;
        break;
    case SETUP_DESCRIPTOR: {
        size_t length = 0;
        const uint8_t *descriptor = hid_host_get_report_descriptor(ctx->device, &length);
        if (!descriptor) { err = ESP_FAIL; break; }
        if (!hid_layout_parse(&ctx->layout, descriptor, length)) {
            ESP_LOGW(TAG, "Unsupported descriptor addr=%u iface=%u (%u bytes)",
                     ctx->params.addr, ctx->params.iface_num, (unsigned)length);
            ctx->stage = INPUT_CLOSE;
            break;
        }
        ESP_LOGI(TAG, "addr=%u iface=%u parsed %u reports, %u fields",
                 ctx->params.addr, ctx->params.iface_num, ctx->layout.reports, ctx->layout.fields);
        ctx->stage = SETUP_START;
        break;
    }
    case SETUP_START:
        portENTER_CRITICAL(&source_lock);
        ctx->layout_ready = !ctx->disconnected;
        // Serialize presence with the disconnect callback's state transition.
        if (ctx->layout_ready) ble_switch_usb_connected(ctx->source);
        portEXIT_CRITICAL(&source_lock);
        err = hid_host_device_start(ctx->device);
        if (err == ESP_OK) {
            ctx->stage = INPUT_RUNNING;
            ctx->failures = 0;
            ESP_LOGI(TAG, "%s addr=%u iface=%u receiving",
                     protocol_name(ctx->params.proto), ctx->params.addr, ctx->params.iface_num);
        } else {
            // start() changes driver state to ACTIVE even when submit fails.
            ctx->stage = INPUT_STOP;
            portENTER_CRITICAL(&source_lock);
            ctx->layout_ready = false;
            portEXIT_CRITICAL(&source_lock);
            ble_switch_release_input(ctx->source);
        }
        break;
    case INPUT_STOP:
        err = hid_host_device_stop(ctx->device);
        if (err == ESP_OK) ctx->stage = SETUP_START;
        break;
    case INPUT_CLOSE:
        err = hid_host_device_close(ctx->device);
        // Successful close invokes DISCONNECTED; the next pass finalizes it.
        break;
    case INPUT_RUNNING: return;
    }
    if (err != ESP_OK) {
        ++ctx->failures;
        ctx->retry_at = esp_timer_get_time() + (ctx->failures < 4 ? 250000 : 2000000);
        if (ctx->failures <= 4 || ctx->failures % 10 == 0)
            ESP_LOGW(TAG, "USB retry addr=%u iface=%u stage=%d attempt=%u: %s",
                     ctx->params.addr, ctx->params.iface_num, ctx->stage, ctx->failures, esp_err_to_name(err));
    } else ctx->retry_at = 0;
}
static int64_t no_input_since, power_on_at;
static void service_usb_commands(void)
{
    portENTER_CRITICAL(&source_lock);
    unsigned commands = usb_commands;
    usb_commands = 0;
    portEXIT_CRITICAL(&source_lock);
    if (!commands) return;
    // Runs in app_main, the only owner of context allocation and teardown.
    for (unsigned i = 0; i < ROUTER_SOURCES; ++i) if (inputs[i].in_use) {
        usb_input_context_t *ctx = &inputs[i];
        portENTER_CRITICAL(&source_lock);
        usb_input_stats_t stats = ctx->stats;
        bool accepting = ctx->layout_ready && !ctx->disconnected && !ctx->recover;
        portEXIT_CRITICAL(&source_lock);
        ESP_LOGI(TAG, "USB source=%u addr=%u iface=%u stage=%d accepting=%d callbacks=%lu decoded=%lu read_errors=%lu decode_errors=%lu transfer_errors=%lu last_callback_ms=%lld",
                 i, ctx->params.addr, ctx->params.iface_num, ctx->stage, accepting,
                 (unsigned long)stats.callbacks, (unsigned long)stats.decoded,
                 (unsigned long)stats.read_errors, (unsigned long)stats.decode_errors,
                 (unsigned long)stats.transfer_errors,
                 stats.callbacks ? (long long)((esp_timer_get_time()-stats.last_callback)/1000) : -1LL);
    }
    ESP_LOGI(TAG, "USB status complete; root_restart_pending=%d", power_on_at != 0);
    usb_health_status();
    if (!(commands & (USB_COMMAND_RESET | USB_COMMAND_AUTO_RESET)) || power_on_at) return;
    esp_err_t err = usb_host_lib_set_root_port_power(false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "USB root restart failed: %s", esp_err_to_name(err));
        return;
    }
    usb_health_reset_started();
    for (unsigned i = 0; i < ROUTER_SOURCES; ++i) if (inputs[i].in_use) {
        portENTER_CRITICAL(&source_lock);
        inputs[i].layout_ready = false;
        inputs[i].reset_wait = true;
        portEXIT_CRITICAL(&source_lock);
        ble_switch_remove(i);
    }
    // Let the normal disconnect callbacks and owning task dispose of handles.
    // This resets the host port, not the external hub's power supply.
    no_input_since = esp_timer_get_time();
    power_on_at = no_input_since + 500000;
    ESP_LOGW(TAG, "%s USB root restart; released input, reconnecting in 500 ms",
             commands & USB_COMMAND_AUTO_RESET ? "Automatic" : "Manual");
}
static bool usb_console_command(const char *line)
{
    unsigned command = !strcmp(line, "usb status") ? USB_COMMAND_STATUS :
                       !strcmp(line, "usb reset") ? USB_COMMAND_RESET : 0;
    if (!command) return false;
    portENTER_CRITICAL(&source_lock);
    usb_commands |= command;
    portEXIT_CRITICAL(&source_lock);
    ESP_LOGI(TAG, "Queued %s", line);
    return true;
}
static void service_root_port(bool running)
{
    int64_t now = esp_timer_get_time();
    if (running) no_input_since = now;
    if (power_on_at && now >= power_on_at) {
        esp_err_t err = usb_host_lib_set_root_port_power(true);
        if (err == ESP_OK) { power_on_at = 0; no_input_since = now; }
    } else if (!power_on_at && !running && now - no_input_since >= 10000000) {
        // Recover enumeration failures that occur before HID CONNECTED.
        // Never reset a hub with a working keyboard or mouse interface.
        esp_err_t err = usb_host_lib_set_root_port_power(false);
        if (err == ESP_OK) {
            usb_health_reset_started();
            power_on_at = now + 500000;
            ESP_LOGW(TAG, "No usable USB input for 10 seconds; restarting root port");
        }
        no_input_since = now;
    }
}
static void usb_host_task(void *arg)
{
    const usb_host_config_t config = {.skip_phy_setup=false, .root_port_unpowered=true, .intr_flags=ESP_INTR_FLAG_LOWMED};
    ESP_ERROR_CHECK(usb_host_install(&config)); xTaskNotifyGive((TaskHandle_t)arg);
    while (true) { uint32_t flags; ESP_ERROR_CHECK(usb_host_lib_handle_events(portMAX_DELAY, &flags)); }
}
static void console_task(void *arg)
{
    char line[24]; unsigned length = 0; bool too_long = false;
    while (true) {
        int c = getchar();
        if (c == EOF) { clearerr(stdin); vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (c == '\r' || c == '\n') {
            if (length && !too_long) {
                line[length] = 0;
                if (!usb_console_command(line)) {
                    if (!strcmp(line, "help")) ESP_LOGI(TAG, "USB commands: usb status | usb reset");
                    ble_switch_command(line);
                }
            }
            length = 0; too_long = false;
        } else if (c == 8 || c == 127) { if (length) --length; }
        else if (c >= 32 && c < 127) {
            if (length < sizeof(line)-1) line[length++] = c; else too_long = true;
        }
    }
}
void app_main(void)
{
    esp_err_t led_err = status_led_init();
    if (led_err != ESP_OK) ESP_LOGW(TAG, "Status LED unavailable: %s", esp_err_to_name(led_err));
    ESP_ERROR_CHECK(ble_switch_init());
    connection_queue = xQueueCreate(32, sizeof(hid_host_device_handle_t));
    ESP_ERROR_CHECK(connection_queue ? ESP_OK : ESP_ERR_NO_MEM);
    BaseType_t created = xTaskCreatePinnedToCore(usb_host_task, "usb_host", 4096, xTaskGetCurrentTaskHandle(), 2, NULL, 0);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    const hid_host_driver_config_t config = {
        .create_background_task=true, .task_priority=5, .stack_size=4096,
        .core_id=0, .callback=device_callback, .callback_arg=NULL,
    };
    ESP_ERROR_CHECK(hid_host_install(&config));
    esp_err_t health_err = usb_health_init();
    if (health_err != ESP_OK) ESP_LOGW(TAG, "USB health monitor unavailable: %s", esp_err_to_name(health_err));
    ESP_ERROR_CHECK(usb_host_lib_set_root_port_power(true));
    created = xTaskCreate(console_task, "switch_console", 3072, NULL, 1, NULL);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_LOGI(TAG, "USB hub -> keyboard/mouse; COM -> logs and commands. Type 'help' then Enter.");
    no_input_since = esp_timer_get_time();
    status_led_boot_complete();
    ESP_LOGI(TAG, "Board initialization complete; LED now shows connection status");
    while (true) {
        hid_host_device_handle_t device;
        if (xQueueReceive(connection_queue, &device, pdMS_TO_TICKS(10)) == pdTRUE) open_device(device);
        service_usb_commands();
        bool running = false;
        for (unsigned i = 0; i < ROUTER_SOURCES; ++i) {
            service_device(&inputs[i]);
            portENTER_CRITICAL(&source_lock);
            bool accepting = inputs[i].in_use && inputs[i].stage == INPUT_RUNNING &&
                       inputs[i].layout_ready && !inputs[i].disconnected && !inputs[i].recover;
            portEXIT_CRITICAL(&source_lock);
            running |= accepting;

        }
        // Aggregate composite keyboard/mouse interfaces once per physical address.
        for (unsigned i=0; i<ROUTER_SOURCES; ++i) {
            if (!inputs[i].in_use) continue;
            uint8_t address = inputs[i].params.addr;
            bool seen = false;
            for (unsigned j=0; j<i; ++j)
                seen |= inputs[j].in_use && inputs[j].params.addr == address;
            if (seen) continue;
            uint32_t reports = 0;
            int64_t last_input = 0;
            bool held = false, active = false;
            portENTER_CRITICAL(&source_lock);
            for (unsigned j=i; j<ROUTER_SOURCES; ++j) {
                usb_input_context_t *ctx = &inputs[j];
                if (!ctx->in_use || ctx->params.addr != address ||
                    !ctx->layout_ready || ctx->disconnected || ctx->recover) continue;
                active = true;
                reports += ctx->stats.callbacks;
                held |= ctx->stats.held;
                if (ctx->stats.last_callback > last_input) last_input = ctx->stats.last_callback;
            }
            portEXIT_CRITICAL(&source_lock);
            if (active) usb_health_watch(address, last_input, reports, held);
        }
        uint8_t leds = ble_switch_leds();
        for (unsigned i=0;i<ROUTER_SOURCES;++i) {
            usb_input_context_t *ctx=&inputs[i];
            portENTER_CRITICAL(&source_lock);
            bool active=ctx->in_use && ctx->layout_ready && !ctx->disconnected && !ctx->recover;
            portEXIT_CRITICAL(&source_lock);
            if (!active || ctx->params.proto!=HID_PROTOCOL_KEYBOARD) continue;
            for(unsigned r=0;r<ctx->layout.reports;++r) {
                uint8_t report[HID_INPUT_BYTES];
                size_t length=hid_layout_led_report(&ctx->layout,r,leds,report);
                if(length) usb_health_leds(ctx->params.addr,ctx->params.iface_num,ctx->layout.ids[r],report,length);
            }
        }
        if (usb_health_poll()) {
            portENTER_CRITICAL(&source_lock);
            usb_commands |= USB_COMMAND_AUTO_RESET;
            portEXIT_CRITICAL(&source_lock);
        }
        service_root_port(running);
    }
}
