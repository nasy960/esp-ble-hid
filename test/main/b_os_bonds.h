#pragma once
#include "esp_err.h"
#include "host/ble_hs.h"
#include <stdbool.h>
esp_err_t b_os_bonds_init(void);
void b_os_bonds_install(void);
bool b_os_bonds_managed(const ble_addr_t *addr);
void b_os_bonds_begin(const ble_addr_t *addr);
bool b_os_bonds_select(const ble_addr_t *addr, const char *name, bool enroll);
esp_err_t b_os_bonds_forget(void);
bool b_os_bonds_ready(const ble_addr_t *addr);
// Existing bonds: choose once per connection; never change a live connection's key.
bool b_os_bonds_prepare(const ble_addr_t *addr);
void b_os_bonds_failed(int reason);
void b_os_bonds_success(void);
void b_os_bonds_retry(void);
