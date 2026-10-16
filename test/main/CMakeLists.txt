idf_component_register(SRCS "hello_world_main.c" "hid_input.c" "input_router.c" "ble_switch.c"
                            "status_led.c" "status_pattern.c" "mouse_pending.c" "keyboard_pending.c" "usb_health.c" "os_profile.c" "b_os_bonds.c" "b_auth_gate.c"
                       PRIV_REQUIRES bt nvs_flash esp_timer mbedtls
                       INCLUDE_DIRS "")

# Isolate SDK NimBLE bonds/IRK/CCCD storage per persistent OS profile.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=nvs_open" "-Wl,--wrap=ble_sm_ltk_req_rx")
