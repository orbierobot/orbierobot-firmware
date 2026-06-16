#include "ota_update.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

static const char *TAG = "ota";

const char *ota_running_version(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    return desc ? desc->version : "unknown";
}

void ota_update_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "Booted a pending-verify image — confirm with ota_mark_valid()");
    }
    ESP_LOGI(TAG, "Running version: %s (partition '%s')",
             ota_running_version(), running->label);
}

void ota_mark_valid(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            ESP_LOGI(TAG, "Image confirmed valid; rollback cancelled");
        } else {
            ESP_LOGE(TAG, "Failed to mark image valid");
        }
    }
}

// --- manifest fetch ---------------------------------------------------------

typedef struct {
    char *buf;
    int   len;
    int   cap;
} resp_buf_t;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data) {
        resp_buf_t *r = (resp_buf_t *)evt->user_data;
        if (r->len + evt->data_len < r->cap) {
            memcpy(r->buf + r->len, evt->data, evt->data_len);
            r->len += evt->data_len;
            r->buf[r->len] = '\0';
        } else {
            ESP_LOGE(TAG, "manifest larger than buffer");
        }
    }
    return ESP_OK;
}

static esp_err_t fetch_manifest(const char *url, char *out, int cap)
{
    resp_buf_t r = { .buf = out, .len = 0, .cap = cap };
    esp_http_client_config_t cfg = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler     = http_event,
        .user_data         = &r,
        .timeout_ms        = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK && status == 200) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "manifest fetch failed: %s (HTTP %d)", esp_err_to_name(err), status);
    return ESP_FAIL;
}

esp_err_t ota_check_and_update(const char *manifest_url)
{
    if (!manifest_url || manifest_url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char json[1024];
    if (fetch_manifest(manifest_url, json, sizeof(json)) != ESP_OK) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGE(TAG, "manifest is not valid JSON");
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_OK;
    const cJSON *jver = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *jurl = cJSON_GetObjectItemCaseSensitive(root, "url");

    if (cJSON_IsString(jver) && cJSON_IsString(jurl)) {
        const char *latest  = jver->valuestring;
        const char *running = ota_running_version();
        ESP_LOGI(TAG, "running=%s  latest=%s", running, latest);

        if (strcmp(latest, running) != 0) {
            ESP_LOGI(TAG, "Updating to %s from %s", latest, jurl->valuestring);
            esp_http_client_config_t http = {
                .url               = jurl->valuestring,
                .crt_bundle_attach = esp_crt_bundle_attach,
                .timeout_ms        = 20000,
                .keep_alive_enable = true,
            };
            esp_https_ota_config_t ota = { .http_config = &http };
            esp_err_t err = esp_https_ota(&ota);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "OTA succeeded — rebooting");
                cJSON_Delete(root);
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
            ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
            ret = err;
        } else {
            ESP_LOGI(TAG, "Firmware already up to date");
        }
    } else {
        ESP_LOGE(TAG, "manifest missing string 'version'/'url'");
        ret = ESP_FAIL;
    }

    cJSON_Delete(root);
    return ret;
}
