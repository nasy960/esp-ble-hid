#include "os_profile.h"
#include "nvs.h"
#include "esp_random.h"
#include <string.h>
static unsigned active;
static uint8_t address[6];
static nvs_handle_t config;
unsigned os_profile_active(void) { return active; }
const char *os_profile_name(void) { return "ESP USB Switch"; }
const char *os_profile_peers_namespace(void) { return active ? "peers_windows" : "peers_ubuntu"; }
const uint8_t *os_profile_address(void) { return address; }
// Linker wrapping scopes *all* NimBLE NVS accesses (including local IRK and
// CCCDs) to the active profile. Other application namespaces pass unchanged.
esp_err_t __real_nvs_open(const char *, nvs_open_mode_t, nvs_handle_t *);
esp_err_t __wrap_nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    if (!strcmp(name, "nimble_bond")) name = active ? "bond_windows" : "bond_ubuntu";
    return __real_nvs_open(name, mode, handle);
}
esp_err_t os_profile_init(void)
{
    esp_err_t err = nvs_open("os_profiles", NVS_READWRITE, &config);
    if (err != ESP_OK) return err;
    uint8_t saved = 0;
    err = nvs_get_u8(config, "active", &saved);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
    if (saved > 1) return ESP_ERR_INVALID_STATE;
    active = saved;
    const char *key = active ? "addr_windows" : "addr_ubuntu";
    size_t size = sizeof(address);
    err = nvs_get_blob(config, key, address, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        esp_fill_random(address, sizeof(address));
        address[5] |= 0xc0; // Persistent static-random identity, little endian.
        address[0] = (address[0] & 0xfe) | active; // Ensure profiles differ.
        address[1] = (address[1] & 0xfe) | 2; // Random portion neither all zero nor all one.
        err = nvs_set_blob(config, key, address, sizeof(address));
        if (err == ESP_OK) err = nvs_commit(config);
    } else if (err == ESP_OK && (size != sizeof(address) || (address[5]&0xc0)!=0xc0)) {
        err = ESP_ERR_INVALID_STATE;
    }
    return err;
}
esp_err_t os_profile_select(unsigned profile)
{
    if (profile > 1) return ESP_ERR_INVALID_ARG;
    esp_err_t err = nvs_set_u8(config, "active", profile);
    if (err == ESP_OK) err = nvs_commit(config);
    // Active identity/storage changes only on reboot, never during live callbacks.
    return err;
}
