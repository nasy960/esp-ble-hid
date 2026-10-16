#pragma once
#include "esp_err.h"
#include <stdint.h>
esp_err_t os_profile_init(void);
unsigned os_profile_active(void);
const char *os_profile_name(void);
const char *os_profile_peers_namespace(void);
const uint8_t *os_profile_address(void);
esp_err_t os_profile_select(unsigned profile);
