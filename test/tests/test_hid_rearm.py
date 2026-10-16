"""Compile the actual driver completion callback with USB failure injection.

Extracting this one static function avoids mocking the unrelated enumeration and
FreeRTOS APIs of the driver. No copy of the callback implementation lives here.
"""
from pathlib import Path
import subprocess
import tempfile

driver = Path(__file__).resolve().parents[1] / "components/usb_host_hid/hid_host.c"
source = driver.read_text()
start = source.index("static void in_xfer_done(usb_transfer_t *in_xfer)")
opening = source.index("{", start)
depth = 1
end = opening + 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
callback = source[start:end]

stub = r'''
#include <assert.h>
#include <stdio.h>
typedef int esp_err_t;
enum { ESP_OK, ESP_FAIL, ESP_ERR_INVALID_STATE, ESP_ERR_NOT_FOUND };
enum { USB_TRANSFER_STATUS_COMPLETED, USB_TRANSFER_STATUS_NO_DEVICE,
       USB_TRANSFER_STATUS_CANCELED, USB_TRANSFER_STATUS_ERROR };
enum { HID_HOST_INTERFACE_EVENT_INPUT_REPORT, HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR };
typedef struct { int unused; } hid_iface_t;
typedef struct { void *context; int status; } usb_transfer_t;
static int result, submissions, count, events[4];
#define ESP_LOGE(...) ((void)0)
static int usb_host_transfer_submit(usb_transfer_t *transfer)
{ assert(transfer); ++submissions; return result; }
static void hid_host_user_interface_callback(hid_iface_t *iface, int event)
{ assert(iface); assert(count < 4); events[count++] = event; }
'''
test = r'''
int main(void)
{
    hid_iface_t iface = {0};
    usb_transfer_t transfer = { .context = &iface };
    for (result = ESP_OK; result <= ESP_ERR_NOT_FOUND; ++result) {
        count = submissions = 0;
        transfer.status = USB_TRANSFER_STATUS_COMPLETED;
        in_xfer_done(&transfer);
        assert(submissions == 1);
        assert(events[0] == HID_HOST_INTERFACE_EVENT_INPUT_REPORT);
        assert(count == (result == ESP_OK ? 1 : 2));
        if (result != ESP_OK)
            assert(events[1] == HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR);
    }
    for (int status = USB_TRANSFER_STATUS_NO_DEVICE;
         status <= USB_TRANSFER_STATUS_ERROR; ++status) {
        count = submissions = 0;
        transfer.status = status;
        in_xfer_done(&transfer);
        assert(submissions == 0);
        assert(count == (status == USB_TRANSFER_STATUS_ERROR ? 1 : 0));
        if (count) assert(events[0] == HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR);
    }
    puts("HID rearm regression passed");
}
'''
with tempfile.TemporaryDirectory(prefix="hid-rearm-") as directory:
    c_file = Path(directory) / "callback.c"
    binary = Path(directory) / "callback"
    c_file.write_text(stub + callback + test)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(c_file), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
