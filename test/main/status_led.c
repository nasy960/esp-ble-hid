#include "status_led.h"
#include "status_pattern.h"
#include "sdkconfig.h"
#if CONFIG_INPUT_SWITCH_LED_ENABLED
#include <string.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"

static const char *TAG = "status_led";
static QueueHandle_t updates;
static led_strip_handle_t strip;
static atomic_bool boot_complete;

static void led_task(void *arg)
{
    status_pattern_t pattern = {.slot=0, .mode=STATUS_NONE}, next;
    int64_t epoch = esp_timer_get_time();
    uint8_t previous[3] = {255,255,255};
    while (true) {
        // Rendering stays off the BLE host task; RMT waits cannot delay HID routing.
        if (xQueueReceive(updates, &next, 0) == pdTRUE &&
            (next.slot != pattern.slot || next.mode != pattern.mode)) {
            pattern = next;
            epoch = esp_timer_get_time();
        }
        uint8_t rgb[3];
        status_pattern_t visible = atomic_load(&boot_complete) ? pattern :
                                   (status_pattern_t){.mode=STATUS_BOOTING};
        status_pattern_rgb(visible, (uint32_t)((esp_timer_get_time()-epoch)/1000),
                           CONFIG_INPUT_SWITCH_LED_BRIGHTNESS, rgb);
        if (memcmp(rgb, previous, sizeof(rgb))) {
            esp_err_t err = led_strip_set_pixel(strip, 0, rgb[0], rgb[1], rgb[2]);
            if (err == ESP_OK) err = led_strip_refresh(strip);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "LED update failed: %s; disabling indicator", esp_err_to_name(err));
                vTaskDelete(NULL);
                return;
            }
            memcpy(previous, rgb, sizeof(rgb));
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}
esp_err_t status_led_init(void)
{
    const led_strip_config_t config = {
        .strip_gpio_num=CONFIG_INPUT_SWITCH_LED_GPIO, .max_leds=1,
        .led_model=LED_MODEL_WS2812,
        .color_component_format=LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rmt = {.resolution_hz=10000000, .flags.with_dma=false};
    esp_err_t err = led_strip_new_rmt_device(&config, &rmt, &strip);
    if (err != ESP_OK) return err;
    // Show startup before BLE/USB initialization can block or fail.
    err = led_strip_set_pixel(strip, 0, CONFIG_INPUT_SWITCH_LED_BRIGHTNESS,
                             CONFIG_INPUT_SWITCH_LED_BRIGHTNESS, 0);
    if (err == ESP_OK) err = led_strip_refresh(strip);
    if (err != ESP_OK) { led_strip_del(strip); strip=NULL; return err; }
    updates = xQueueCreate(1, sizeof(status_pattern_t));
    if (!updates) { led_strip_del(strip); strip=NULL; return ESP_ERR_NO_MEM; }
    if (xTaskCreate(led_task, "status_led", 3072, NULL, 1, NULL) != pdPASS) {
        vQueueDelete(updates); updates=NULL; led_strip_del(strip); strip=NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "WS2812 GPIO%d brightness=%d/255; boot=yellow A=green B=blue",
             CONFIG_INPUT_SWITCH_LED_GPIO, CONFIG_INPUT_SWITCH_LED_BRIGHTNESS);
    return ESP_OK;
}
void status_led_boot_complete(void)
{
    atomic_store(&boot_complete, true);
}
void status_led_update(unsigned selected, bool selected_ready, int pairing_slot, bool no_devices)
{
    if (updates) {
        status_pattern_t pattern = status_pattern_select(selected, selected_ready, pairing_slot, no_devices);
        xQueueOverwrite(updates, &pattern);
    }
}
#else
esp_err_t status_led_init(void) { return ESP_OK; }
void status_led_boot_complete(void) {}
void status_led_update(unsigned selected, bool selected_ready, int pairing_slot, bool no_devices) {}
#endif
