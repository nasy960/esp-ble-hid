# Project-local USB HID driver

Based on Espressif `usb_host_hid` 1.2.1, esp-usb commit
`e3c35b840b9fc9969b981011c9ff782c01168433`, directory
`host/class/hid/usb_host_hid`. Original license is retained in LICENSE.

The main component manifest overrides the registry dependency with this directory,
so clean builds preserve the fix. Do not edit the generated managed component.

Local change: `in_xfer_done` checks interrupt IN resubmission. Failure logs its
error and delivers TRANSFER_ERROR, allowing the application's existing deferred
stop/start recovery to release cached input and resume polling. Successful,
canceled, and disconnected transfers retain their original behavior.

Regression: `python3 test/tests/test_hid_rearm.py` from the repository root.
When updating upstream, review this patch and rerun the regression and IDF build.
