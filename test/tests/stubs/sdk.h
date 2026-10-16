#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_NO_MEM 2
#define ESP_ERR_TIMEOUT 3
#define ESP_INTR_FLAG_LOWMED 0
#define ESP_ERROR_CHECK(e) assert((e) == ESP_OK)
const char *esp_err_to_name(esp_err_t e);
int64_t esp_timer_get_time(void);
static inline void test_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
#define ESP_LOGI test_log
#define ESP_LOGW test_log
#define ESP_LOGE test_log
#define ESP_LOGD test_log
#define ESP_LOG_INFO 0
#define ESP_LOG_BUFFER_HEX_LEVEL(...) ((void)0)
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
typedef int BaseType_t;
typedef void *TaskHandle_t;
typedef void *QueueHandle_t;
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)
void vTaskDelay(unsigned ticks);
void xTaskNotifyGive(TaskHandle_t task);
unsigned ulTaskNotifyTake(int clear, unsigned ticks);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
BaseType_t xTaskCreate(void (*fn)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *);
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *, int);
QueueHandle_t xQueueCreate(unsigned, unsigned);
BaseType_t xQueueSend(QueueHandle_t, const void *, unsigned);
BaseType_t xQueueReceive(QueueHandle_t, void *, unsigned);
typedef void *hid_host_device_handle_t;
typedef enum { HID_PROTOCOL_NONE, HID_PROTOCOL_KEYBOARD, HID_PROTOCOL_MOUSE } hid_protocol_t;
typedef enum { HID_HOST_INTERFACE_EVENT_INPUT_REPORT, HID_HOST_INTERFACE_EVENT_DISCONNECTED,
               HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR } hid_host_interface_event_t;
typedef enum { HID_HOST_DRIVER_EVENT_CONNECTED } hid_host_driver_event_t;
#define HID_SUBCLASS_BOOT_INTERFACE 1
#define HID_REPORT_PROTOCOL_REPORT 1
typedef struct { uint8_t addr, iface_num, sub_class; hid_protocol_t proto; } hid_host_dev_params_t;
typedef struct { uint16_t VID, PID; } hid_host_dev_info_t;
typedef struct { void (*callback)(hid_host_device_handle_t, hid_host_interface_event_t, void *); void *callback_arg; } hid_host_device_config_t;
typedef struct { bool create_background_task; unsigned task_priority, stack_size; int core_id;
    void (*callback)(hid_host_device_handle_t, hid_host_driver_event_t, void *); void *callback_arg; } hid_host_driver_config_t;
typedef struct { bool skip_phy_setup, root_port_unpowered; int intr_flags; } usb_host_config_t;
esp_err_t usb_host_install(const usb_host_config_t *);
esp_err_t usb_host_lib_handle_events(unsigned, uint32_t *);
esp_err_t usb_host_lib_set_root_port_power(bool);
esp_err_t hid_host_install(const hid_host_driver_config_t *);
esp_err_t hid_host_device_get_params(hid_host_device_handle_t, hid_host_dev_params_t *);
esp_err_t hid_host_get_device_info(hid_host_device_handle_t, hid_host_dev_info_t *);
esp_err_t hid_host_device_open(hid_host_device_handle_t, const hid_host_device_config_t *);
esp_err_t hid_host_device_close(hid_host_device_handle_t);
esp_err_t hid_host_device_start(hid_host_device_handle_t);
esp_err_t hid_host_device_stop(hid_host_device_handle_t);
esp_err_t hid_class_request_set_protocol(hid_host_device_handle_t, int);
esp_err_t hid_class_request_set_idle(hid_host_device_handle_t, uint8_t, uint8_t);
const uint8_t *hid_host_get_report_descriptor(hid_host_device_handle_t, size_t *);
esp_err_t hid_host_device_get_raw_input_report_data(hid_host_device_handle_t, uint8_t *, size_t, size_t *);
typedef void *usb_device_handle_t;
typedef void *usb_host_client_handle_t;
typedef struct usb_transfer_s {
    uint8_t *data_buffer;
    int num_bytes, actual_num_bytes, status;
    uint8_t bEndpointAddress;
    usb_device_handle_t device_handle;
    void (*callback)(struct usb_transfer_s *);
    void *context;
} usb_transfer_t;
enum { USB_TRANSFER_STATUS_COMPLETED, USB_TRANSFER_STATUS_ERROR,
       USB_TRANSFER_STATUS_TIMED_OUT, USB_TRANSFER_STATUS_CANCELED,
       USB_TRANSFER_STATUS_STALL, USB_TRANSFER_STATUS_OVERFLOW,
       USB_TRANSFER_STATUS_SKIPPED, USB_TRANSFER_STATUS_NO_DEVICE };
typedef struct { uint8_t bmRequestType, bRequest; uint16_t wValue, wIndex, wLength; } usb_setup_packet_t;
#define USB_SETUP_PACKET_SIZE 8
#define USB_SETUP_PACKET_INIT_GET_STATUS(s) (*(s)=(usb_setup_packet_t){.bmRequestType=0x80,.wLength=2})
enum { USB_HOST_CLIENT_EVENT_NEW_DEV, USB_HOST_CLIENT_EVENT_DEV_GONE };
typedef struct { int event; struct { usb_device_handle_t dev_hdl; } dev_gone; } usb_host_client_event_msg_t;
typedef struct {
    bool is_synchronous; int max_num_event_msg;
    struct { void (*client_event_callback)(const usb_host_client_event_msg_t *, void *); void *callback_arg; } async;
} usb_host_client_config_t;
esp_err_t usb_host_client_register(const usb_host_client_config_t *, usb_host_client_handle_t *);
esp_err_t usb_host_client_handle_events(usb_host_client_handle_t, unsigned);
esp_err_t usb_host_device_open(usb_host_client_handle_t, uint8_t, usb_device_handle_t *);
esp_err_t usb_host_device_close(usb_host_client_handle_t, usb_device_handle_t);
esp_err_t usb_host_transfer_alloc(size_t, int, usb_transfer_t **);
esp_err_t usb_host_transfer_free(usb_transfer_t *);
esp_err_t usb_host_transfer_submit_control(usb_host_client_handle_t, usb_transfer_t *);
