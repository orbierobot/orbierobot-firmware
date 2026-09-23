#include "voice_link.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "voice_link";

/* Two seconds is a compromise: fast enough that an answer does not feel
 * stale, slow enough that a robot left running all day is not hammering a
 * serverless function 43,000 times an hour. */
#define POLL_INTERVAL_MS   2000
/* A frame every 15s keeps "what can you see" roughly current without spending
 * the whole uplink on JPEGs. A push-to-talk press also forces one. */
#define FRAME_INTERVAL_MS  15000
#define HTTP_TIMEOUT_MS    15000

static char s_api[96]  = "";
static char s_robot[24] = "";
static char s_key[64]  = "";
static voice_play_fn          s_play;
static voice_frame_fn         s_grab;
static voice_frame_release_fn s_release;

static volatile bool s_online = false;
static volatile bool s_want_frame = false;
static char s_err[96] = "";

bool voice_link_online(void)           { return s_online; }
const char *voice_link_last_error(void){ return s_err; }
void voice_link_request_frame(void)    { s_want_frame = true; }

static void set_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_err, sizeof(s_err), fmt, ap);
    va_end(ap);
}

static void add_key(esp_http_client_handle_t c)
{
    if (s_key[0]) esp_http_client_set_header(c, "X-Orbie-Key", s_key);
}

/* ------------------------------------------------------------------ poll */

/* Streams the body straight to the speaker rather than buffering it. A 20s
 * answer is 640KB of PCM - it would not fit in RAM, and waiting for the whole
 * clip before starting would add that much latency for no benefit. */
static void poll_once(void)
{
    char url[200];
    snprintf(url, sizeof(url), "%s/api/pending?robot=%s", s_api, s_robot);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,  /* trust the usual CAs */
        .buffer_size = 2048,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return;
    add_key(c);

    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        set_err("connect: %s", esp_err_to_name(err));
        s_online = false;
        esp_http_client_cleanup(c);
        return;
    }

    int64_t len = esp_http_client_fetch_headers(c);
    int status  = esp_http_client_get_status_code(c);
    s_online = true;
    s_err[0] = '\0';

    if (status == 204) {                 /* nothing waiting - the common case */
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return;
    }
    if (status != 200) {
        set_err("pending: HTTP %d", status);
        ESP_LOGW(TAG, "pending returned %d", status);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return;
    }

    ESP_LOGI(TAG, "answer waiting: %lld bytes (%.1fs)", (long long)len,
             len > 0 ? len / 32000.0f : 0.0f);

    uint8_t buf[1024];
    bool first = true;
    int total = 0;
    while (true) {
        int n = esp_http_client_read(c, (char *)buf, sizeof(buf));
        if (n <= 0) break;
        if (s_play) s_play(buf, n, first);
        first = false;
        total += n;
    }
    ESP_LOGI(TAG, "played %d bytes", total);

    esp_http_client_close(c);
    esp_http_client_cleanup(c);
}

/* ----------------------------------------------------------------- frame */

static void push_frame(void)
{
    if (!s_grab) return;

    size_t len = 0;
    const uint8_t *jpeg = s_grab(&len);
    if (!jpeg || len == 0) {
        if (s_release) s_release();
        return;
    }

    char url[200];
    snprintf(url, sizeof(url), "%s/api/frame?robot=%s", s_api, s_robot);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { if (s_release) s_release(); return; }

    esp_http_client_set_header(c, "Content-Type", "image/jpeg");
    add_key(c);

    esp_err_t err = esp_http_client_open(c, len);
    if (err == ESP_OK) {
        int wrote = esp_http_client_write(c, (const char *)jpeg, len);
        esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        if (status == 200) {
            ESP_LOGI(TAG, "frame uploaded (%d bytes)", wrote);
        } else {
            set_err("frame: HTTP %d", status);
            ESP_LOGW(TAG, "frame upload returned %d", status);
        }
        esp_http_client_close(c);
    } else {
        set_err("frame connect: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(c);
    if (s_release) s_release();
}

/* ------------------------------------------------------------------ task */

static void voice_task(void *pv)
{
    ESP_LOGI(TAG, "polling %s as %s", s_api, s_robot);
    int since_frame = 0;

    while (true) {
        poll_once();

        since_frame += POLL_INTERVAL_MS;
        if (s_want_frame || since_frame >= FRAME_INTERVAL_MS) {
            s_want_frame = false;
            since_frame  = 0;
            push_frame();
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

void voice_link_start(const char *api_base,
                      const char *robot_id,
                      const char *device_key,
                      voice_play_fn play_pcm,
                      voice_frame_fn grab_jpeg,
                      voice_frame_release_fn release_jpeg)
{
    strlcpy(s_api,   api_base  ? api_base  : "", sizeof(s_api));
    strlcpy(s_robot, robot_id  ? robot_id  : "", sizeof(s_robot));
    strlcpy(s_key,   device_key ? device_key : "", sizeof(s_key));
    s_play    = play_pcm;
    s_grab    = grab_jpeg;
    s_release = release_jpeg;

    if (!s_api[0] || !s_robot[0]) {
        ESP_LOGW(TAG, "not starting: api_base or robot id is empty");
        return;
    }
    /* 8KB: TLS needs a few KB of stack on top of the HTTP client. */
    xTaskCreate(voice_task, "voice_link", 8192, NULL, 4, NULL);
}
