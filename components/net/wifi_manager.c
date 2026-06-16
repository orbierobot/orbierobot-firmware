#include "wifi_manager.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "wifi";

#define NVS_NS     "openpaw_wifi"
#define KEY_SSID   "ssid"
#define KEY_PASS   "pass"
#define CONNECTED_BIT BIT0

static EventGroupHandle_t s_eg;
static wifi_manager_connected_cb_t s_on_connected;
static bool s_have_creds;
static int  s_retries;
static char s_ip[16];

// Load credentials, preferring NVS and falling back to the Kconfig dev values.
static bool load_creds(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t sl = ssid_cap, pl = pass_cap;
        esp_err_t e1 = nvs_get_str(h, KEY_SSID, ssid, &sl);
        esp_err_t e2 = nvs_get_str(h, KEY_PASS, pass, &pl);
        nvs_close(h);
        if (e1 == ESP_OK && e2 == ESP_OK && ssid[0] != '\0') {
            ESP_LOGI(TAG, "Using stored credentials (ssid=%s)", ssid);
            return true;
        }
    }
    if (CONFIG_OPENPAW_WIFI_SSID[0] != '\0') {
        strncpy(ssid, CONFIG_OPENPAW_WIFI_SSID, ssid_cap - 1); ssid[ssid_cap - 1] = '\0';
        strncpy(pass, CONFIG_OPENPAW_WIFI_PASS, pass_cap - 1); pass[pass_cap - 1] = '\0';
        ESP_LOGW(TAG, "Using dev credentials from Kconfig (ssid=%s)", ssid);
        return true;
    }
    return false;
}

static void apply_sta_config(const char *ssid, const char *pass)
{
    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN; // accept open or secured APs
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    s_have_creds = (ssid[0] != '\0');
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_have_creds) {
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "No credentials yet — waiting for provisioning");
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_eg, CONNECTED_BIT);
        s_ip[0] = '\0';
        if (s_have_creds) {
            s_retries++;
            ESP_LOGW(TAG, "Disconnected; reconnecting (attempt %d)", s_retries);
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&evt->ip_info.ip));
        ESP_LOGI(TAG, "Connected, IP %s", s_ip);
        s_retries = 0;
        xEventGroupSetBits(s_eg, CONNECTED_BIT);
        if (s_on_connected) {
            s_on_connected();
        }
    }
}

void wifi_manager_init(wifi_manager_connected_cb_t on_connected)
{
    s_on_connected = on_connected;
    s_eg = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t loop = esp_event_loop_create_default();
    if (loop != ESP_OK && loop != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(loop);
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &on_event, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    char ssid[33] = { 0 }, pass[65] = { 0 };
    if (load_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        apply_sta_config(ssid, pass);
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    // Keep the radio awake: modem-sleep adds ~100ms+ latency that makes the
    // MJPEG stream and 500ms status polling laggy during live control.
    esp_wifi_set_ps(WIFI_PS_NONE);
}

esp_err_t wifi_manager_set_credentials(const char *ssid, const char *pass)
{
    if (!ssid || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_str(h, KEY_SSID, ssid);
    nvs_set_str(h, KEY_PASS, pass ? pass : "");
    err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "Stored new credentials (ssid=%s); reconnecting", ssid);
    s_retries = 0;
    apply_sta_config(ssid, pass ? pass : "");
    esp_wifi_disconnect();
    return esp_wifi_connect();
}

bool wifi_manager_is_connected(void)
{
    return s_eg && (xEventGroupGetBits(s_eg) & CONNECTED_BIT);
}

void wifi_manager_get_ip(char *buf, size_t cap)
{
    if (!buf || cap == 0) {
        return;
    }
    strncpy(buf, s_ip, cap - 1);
    buf[cap - 1] = '\0';
}
