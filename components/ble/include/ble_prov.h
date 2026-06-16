// OpenPaw BLE provisioning + control service (NimBLE).
//
// Advertises an "OpenPaw-XXYY" peripheral with one GATT service:
//   - CRED   (write)        : JSON {"ssid":"..","pass":".."} -> wifi_manager
//   - STATUS (read/notify)  : 1 byte ble_prov_status_t (Wi-Fi connection state)
//   - INFO   (read)         : "<fw-version>|<sta-mac>" for the app / OTA trigger
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BLE_PROV_IDLE       = 0,
    BLE_PROV_CONNECTING = 1,
    BLE_PROV_CONNECTED  = 2,
    BLE_PROV_FAILED     = 3,
} ble_prov_status_t;

// Start the BLE stack, GATT service, and advertising. Call once after Wi-Fi init.
void ble_prov_init(void);

// Update the STATUS characteristic and notify any subscribed client.
void ble_prov_set_status(ble_prov_status_t status);

#ifdef __cplusplus
}
#endif
