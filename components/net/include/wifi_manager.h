// OpenPaw Wi-Fi station manager.
// Connects in STA mode using credentials from NVS, falling back to the dev
// credentials compiled in via Kconfig. BLE provisioning calls
// wifi_manager_set_credentials() to store new creds and (re)connect.
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Called from the Wi-Fi event task once an IP has been acquired. Keep it short.
typedef void (*wifi_manager_connected_cb_t)(void);

// Bring up Wi-Fi in station mode. Non-blocking: if credentials are available
// (NVS or Kconfig) it begins connecting and invokes on_connected on success.
// If no credentials exist, it stays idle until wifi_manager_set_credentials().
void wifi_manager_init(wifi_manager_connected_cb_t on_connected);

// Persist new credentials to NVS and (re)connect. Safe to call after init,
// e.g. from a BLE characteristic write handler.
esp_err_t wifi_manager_set_credentials(const char *ssid, const char *pass);

// True once the station has an IP address.
bool wifi_manager_is_connected(void);

#ifdef __cplusplus
}
#endif
