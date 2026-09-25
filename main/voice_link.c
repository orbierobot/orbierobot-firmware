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
/* Ceiling for the failure backoff (see voice_task). A minute is slow enough
 * that an unreachable cloud costs almost no sockets, and quick enough that a
 * robot recovers on its own shortly after the network comes back. */
#define MAX_BACKOFF_MS     60000
/* A frame every 15s keeps "what can you see" roughly current without spending
 * the whole uplink on JPEGs. A push-to-talk press also forces one. */
#define FRAME_INTERVAL_MS  15000
#define HTTP_TIMEOUT_MS    15000

static char s_api[96]  = "";
static char s_robot[24] = "";
static char s_key[64]  = "";
static voice_play_fn          s_play;
static voice_play_done_fn     s_done;
static voice_frame_fn         s_grab;
static voice_frame_release_fn s_release;

static volatile bool s_online = false;
static volatile bool s_want_frame = false;
static char s_err[96] = "";

bool voice_link_online(void)           { return s_online; }
const char *voice_link_last_error(void){ return s_err; }
void voice_link_set_key(const char *device_key)
{
    strlcpy(s_key, device_key ? device_key : "", sizeof(s_key));
}

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
    /* Always, even when nothing arrived: the player takes the speaker on the
       first chunk and has no other way to learn the clip is over. Skipping
       this left the audio mutex held for the rest of the boot, so the first
       answer played and every sound after it failed as "busy". */
    if (!first && s_done) s_done();
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
    int backoff_ms  = POLL_INTERVAL_MS;

    while (true) {
        poll_once();

        /* Back off while the cloud is unreachable.
         *
         * This loop used to retry every 2s forever. Each attempt takes a
         * socket from a pool of 16 that the two HTTP servers are already
         * most of the way through, and a failed connection leaves its socket
         * in TIME_WAIT for a good while after. Once the cloud stopped
         * answering, the retries alone drained the pool in a couple of
         * minutes: accept() started returning ENFILE, the control page and
         * camera stopped responding, and TLS could not even allocate - which
         * read like a memory or certificate fault rather than a robot that
         * had simply run out of sockets.
         *
         * Steady state is unchanged: while calls succeed the interval stays
         * at POLL_INTERVAL_MS, so answers arrive just as quickly. */
        if (s_online) {
            backoff_ms = POLL_INTERVAL_MS;
        } else if (backoff_ms < MAX_BACKOFF_MS) {
            backoff_ms *= 2;
            if (backoff_ms > MAX_BACKOFF_MS) backoff_ms = MAX_BACKOFF_MS;
        }

        since_frame += backoff_ms;
        /* No point pushing frames at a server we cannot reach - that is a
         * second socket per round on top of the poll. */
        if (s_online && (s_want_frame || since_frame >= FRAME_INTERVAL_MS)) {
            s_want_frame = false;
            since_frame  = 0;
            push_frame();
        }
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
    }
}

void voice_link_start(const char *api_base,
                      const char *robot_id,
                      const char *device_key,
                      voice_play_fn play_pcm,
                      voice_play_done_fn play_done,
                      voice_frame_fn grab_jpeg,
                      voice_frame_release_fn release_jpeg)
{
    strlcpy(s_api,   api_base  ? api_base  : "", sizeof(s_api));
    strlcpy(s_robot, robot_id  ? robot_id  : "", sizeof(s_robot));
    strlcpy(s_key,   device_key ? device_key : "", sizeof(s_key));
    s_play    = play_pcm;
    s_done    = play_done;
    s_grab    = grab_jpeg;
    s_release = release_jpeg;

    if (!s_api[0] || !s_robot[0]) {
        ESP_LOGW(TAG, "not starting: api_base or robot id is empty");
        return;
    }
    /* 8KB: TLS needs a few KB of stack on top of the HTTP client. */
    xTaskCreate(voice_task, "voice_link", 8192, NULL, 4, NULL);
}

bool voice_link_say(const char *text)
{
    if (!text || !text[0] || !s_api[0] || !s_play) return false;

    ESP_LOGI(TAG, "announcing: \"%.40s\"", text);

    char url[200];
    snprintf(url, sizeof(url), "%s/api/speak", s_api);

    /* The server caps the text, but bound it here too - a runaway caller
     * should not spend money on a minute of synthesis. */
    char body[320];
    int n = snprintf(body, sizeof(body), "{\"text\":\"%.200s\"}", text);
    if (n <= 0 || n >= (int)sizeof(body)) return false;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 45000,             /* synthesis is slower than a poll */
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;
    add_key(c);
    esp_http_client_set_header(c, "Content-Type", "application/json");

    bool ok = false;
    esp_err_t oerr = esp_http_client_open(c, n);
    if (oerr != ESP_OK) {
        ESP_LOGW(TAG, "speak: open failed: %s", esp_err_to_name(oerr));
    } else if (esp_http_client_write(c, body, n) != n) {
        ESP_LOGW(TAG, "speak: write failed");
    } else {
        esp_http_client_fetch_headers(c);
        if (esp_http_client_get_status_code(c) == 200) {
            /* static: this runs on a task that also carries an mbedTLS
             * handshake, and a kilobyte of stack is worth not spending. Only
             * ever one caller at a time. */
            static uint8_t buf[1024];
            bool first = true;
            int total = 0;
            while (true) {
                int r = esp_http_client_read(c, (char *)buf, sizeof(buf));
                if (r <= 0) break;
                s_play(buf, r, first);
                first = false;
                total += r;
            }
            if (!first && s_done) s_done();
            ok = total > 0;
            ESP_LOGI(TAG, "said %d bytes", total);
        } else {
            ESP_LOGW(TAG, "speak returned %d", esp_http_client_get_status_code(c));
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ok;
}
