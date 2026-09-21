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
#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "ble_switch.h"
#include "input_router.h"
#include "status_led.h"

static const char *TAG = "usb_input";
static QueueHandle_t connection_queue;
static portMUX_TYPE source_lock = portMUX_INITIALIZER_UNLOCKED;
typedef struct {
    bool in_use, layout_ready;
    unsigned source;
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
static void release_context(usb_input_context_t *ctx)
{
    ble_switch_remove(ctx->source);
    portENTER_CRITICAL(&source_lock); ctx->in_use = false; portEXIT_CRITICAL(&source_lock);
}
static usb_input_context_t *allocate_context(void)
{
    usb_input_context_t *ctx = NULL;
    portENTER_CRITICAL(&source_lock);
    for (unsigned i = 0; i < ROUTER_SOURCES; ++i) if (!inputs[i].in_use) {
        // Only this task allocates; a disconnected source is never freed from heap.
        inputs[i].in_use = true; ctx = &inputs[i]; break;
    }
    portEXIT_CRITICAL(&source_lock);
    if (ctx) {
        ctx->layout_ready = false; ctx->source = ctx - inputs;
        memset(ctx->keys, 0, sizeof(ctx->keys)); memset(ctx->buttons, 0, sizeof(ctx->buttons));
    }
    return ctx;
}
static void interface_callback(hid_host_device_handle_t device,
                               hid_host_interface_event_t event, void *arg)
{
    usb_input_context_t *ctx = arg;
    hid_host_dev_params_t params;
    esp_err_t err = hid_host_device_get_params(device, &params);
    if (err != ESP_OK) return;
    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT: {
        uint8_t data[HID_INPUT_BYTES]; size_t length = 0;
        err = hid_host_device_get_raw_input_report_data(device, data, sizeof(data), &length);
        if (err != ESP_OK || !length || !ctx->layout_ready) break;
        hid_input_t input;
        if (!hid_layout_decode(&ctx->layout, data, length, &input)) {
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
        ble_switch_input(ctx->source, &input);
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
        release_context(ctx);
        err = hid_host_device_close(device);
        if (err != ESP_OK) ESP_LOGW(TAG, "Close after disconnect: %s", esp_err_to_name(err));
        break;
    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        ble_switch_release_input(ctx->source); // Release keys, but the USB device is still present.
        memset(ctx->keys, 0, sizeof(ctx->keys)); memset(ctx->buttons, 0, sizeof(ctx->buttons));
        ESP_LOGW(TAG, "%s addr=%u iface=%u transfer error; released input", protocol_name(params.proto), params.addr, params.iface_num);
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
    // Register before open: even a disconnect during descriptor/protocol setup
    // must be the last writer of this source's presence state.
    ble_switch_usb_connected(ctx->source);
    const hid_host_device_config_t config = {.callback=interface_callback, .callback_arg=ctx};
    err = hid_host_device_open(device, &config);
    if (err != ESP_OK) {
        release_context(ctx);
        ESP_LOGW(TAG, "Cannot open addr=%u iface=%u: %s", params.addr, params.iface_num, esp_err_to_name(err)); return;
    }
    if (params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
        // Report protocol is required for side buttons, wheels and NKRO keyboards.
        err = hid_class_request_set_protocol(device, HID_REPORT_PROTOCOL_REPORT);
        if (err != ESP_OK) goto failed; // Never apply a Report descriptor to unknown Boot data.
        ESP_LOGI(TAG, "addr=%u iface=%u protocol set to REPORT", params.addr, params.iface_num);
        if (params.proto == HID_PROTOCOL_KEYBOARD) {
            esp_err_t idle_err = hid_class_request_set_idle(device, 0, 0);
            if (idle_err != ESP_OK) ESP_LOGW(TAG, "SET_IDLE: %s", esp_err_to_name(idle_err));
        }
    }
    size_t length = 0;
    const uint8_t *descriptor = hid_host_get_report_descriptor(device, &length);
    if (!descriptor || !hid_layout_parse(&ctx->layout, descriptor, length)) {
        ESP_LOGW(TAG, "Unsupported descriptor addr=%u iface=%u (%u bytes)", params.addr, params.iface_num, (unsigned)length);
        if (descriptor) ESP_LOG_BUFFER_HEX_LEVEL(TAG, descriptor, length, ESP_LOG_INFO);
        err = ESP_ERR_NOT_SUPPORTED; goto failed;
    }
    ESP_LOGI(TAG, "addr=%u iface=%u parsed %u reports, %u fields", params.addr, params.iface_num, ctx->layout.reports, ctx->layout.fields);
    if (params.proto == HID_PROTOCOL_MOUSE) {
        ESP_LOGI(TAG, "Mouse report descriptor (%u bytes); switch button=%d", (unsigned)length, CONFIG_INPUT_SWITCH_BUTTON);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, descriptor, length, ESP_LOG_INFO);
    }
    ctx->layout_ready = true;
    err = hid_host_device_start(device);
    if (err == ESP_OK) { ESP_LOGI(TAG, "%s addr=%u iface=%u connected", protocol_name(params.proto), params.addr, params.iface_num); return; }
failed:
    ESP_LOGW(TAG, "Cannot start addr=%u iface=%u: %s", params.addr, params.iface_num, esp_err_to_name(err));
    // Successful close emits DISCONNECTED; that callback releases the source.
    err = hid_host_device_close(device);
    if (err != ESP_OK) ESP_LOGW(TAG, "Close failed: %s", esp_err_to_name(err));
}
static void usb_host_task(void *arg)
{
    const usb_host_config_t config = {.skip_phy_setup=false, .intr_flags=ESP_INTR_FLAG_LOWMED};
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
            if (length && !too_long) { line[length] = 0; ble_switch_command(line); }
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
    connection_queue = xQueueCreate(10, sizeof(hid_host_device_handle_t));
    ESP_ERROR_CHECK(connection_queue ? ESP_OK : ESP_ERR_NO_MEM);
    BaseType_t created = xTaskCreatePinnedToCore(usb_host_task, "usb_host", 4096, xTaskGetCurrentTaskHandle(), 2, NULL, 0);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    const hid_host_driver_config_t config = {
        .create_background_task=true, .task_priority=5, .stack_size=4096,
        .core_id=0, .callback=device_callback, .callback_arg=NULL,
    };
    ESP_ERROR_CHECK(hid_host_install(&config));
    created = xTaskCreate(console_task, "switch_console", 3072, NULL, 1, NULL);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_LOGI(TAG, "USB hub -> keyboard/mouse; COM -> logs and commands. Type 'help' then Enter.");
    while (true) {
        hid_host_device_handle_t device;
        if (xQueueReceive(connection_queue, &device, portMAX_DELAY) == pdTRUE) open_device(device);
    }
}
