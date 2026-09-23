#pragma once
/* Captive-portal Wi-Fi provisioning for Orbie.
 *
 * The robot always runs its own AP (ORBIE_XXXX) so the control UI and camera
 * stream stay reachable. This module adds a second role: joining the user's
 * home network so the robot gets internet access.
 *
 * Flow:
 *   1. Phone joins ORBIE_XXXX.
 *   2. The phone's OS probes a known URL to test for internet. We hijack DNS
 *      (every name resolves to 192.168.4.1) and answer those probes with a
 *      redirect, which makes iOS/Android pop the portal sheet automatically.
 *   3. The portal lists nearby networks; the user picks one and enters a
 *      password.
 *   4. Credentials are saved to NVS and the STA interface connects. On the
 *      next boot it reconnects on its own.
 *
 * Wi-Fi must be in APSTA mode for this to work - scanning and joining need the
 * STA interface, and keeping the AP up means the user never loses the control
 * UI mid-setup.
 */

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ORBIE_WIFI_IDLE = 0,      /* no credentials stored          */
    ORBIE_WIFI_CONNECTING,    /* association in progress        */
    ORBIE_WIFI_CONNECTED,     /* joined, got an IP              */
    ORBIE_WIFI_FAILED,        /* wrong password / AP not found  */
} orbie_wifi_state_t;

/**
 * @brief Register event handlers and, if credentials are stored in NVS,
 *        start connecting to the saved network. Call once, after
 *        esp_wifi_start() and with the mode already set to APSTA.
 */
void wifi_portal_init(void);

/**
 * @brief Start the DNS hijack task: answers every A query with 192.168.4.1 so
 *        the phone's captive-portal probe lands on us.
 */
void wifi_portal_start_dns(void);

/**
 * @brief Register the portal pages, the OS probe endpoints and the 404
 *        catch-all redirect on an already-started server.
 *        Needs 11 free URI handler slots.
 */
esp_err_t wifi_portal_register(httpd_handle_t server);

/** @brief Current STA state, for the control UI's status line. */
orbie_wifi_state_t wifi_portal_state(void);

/* ---- Used by BLE provisioning -------------------------------------------
 *
 * The same three operations the captive portal exposes over HTTP, callable
 * directly. BLE provisioning needs them before the robot is on any network at
 * all, which is the whole point: the phone can set up Wi-Fi without first
 * joining the robot's AP.
 */

/** @brief Remember these credentials and start connecting. Same path as the
 *         portal's /api/connect, so a BLE-provisioned network is stored and
 *         retried on boot exactly like one entered in the web form. */
void wifi_portal_set_credentials(const char *ssid, const char *pass);

/** @brief Scan and write a JSON array of {ssid,rssi} into @p out. Blocking. */
void wifi_portal_scan_json(char *out, size_t out_len);

/** @brief SSID of the network we joined, or "" when not connected. */
const char *wifi_portal_ssid(void);

/** @brief Dotted-quad STA IP address, or "" when not connected. */
const char *wifi_portal_ip(void);

#ifdef __cplusplus
}
#endif
