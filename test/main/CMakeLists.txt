idf_component_register(SRCS "hello_world_main.c" "hid_input.c" "input_router.c" "ble_switch.c"
                            "status_led.c" "status_pattern.c" "mouse_pending.c"
                       PRIV_REQUIRES bt nvs_flash esp_timer
                       INCLUDE_DIRS "")
