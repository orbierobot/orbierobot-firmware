#include <string.h>
#include <math.h>
#include "esp_camera.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "VL53L0X.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_psram.h"
#include "cJSON.h"
#include "esp_rom_sys.h"
#include "esp_heap_caps.h"
#include "i2c_lock.h"
#include "led_display.h"
#include "boot_sound.h"
#include "control_page.h"
#include "voice_link.h"
#include "ble_prov.h"
#include <stdarg.h>
#include "wifi_portal.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "lwip/sockets.h"
#include "soc/usb_serial_jtag_reg.h"
#include "soc/usb_serial_jtag_struct.h"

static const char *TAG = "camera_web";
static i2c_master_bus_handle_t i2c_bus = NULL;
static volatile uint16_t g_distance_mm = 0;
static volatile bool g_laser_enabled = false;
static volatile bool g_laser_auto = false;

/* MLX90614 IR temperature sensor */
#define MLX90614_ADDR    0x5A
static i2c_master_dev_handle_t temp_dev = NULL;
static volatile float temp_ambient = 0.0f;
static volatile float temp_object = 0.0f;
static volatile bool temp_ok = false;

#define LASER_GPIO GPIO_NUM_9

/* Camera pins (XIAO ESP32S3 Sense) */
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     10
#define SIOD_GPIO_NUM     40
#define SIOC_GPIO_NUM     39
#define Y9_GPIO_NUM       48
#define Y8_GPIO_NUM       11
#define Y7_GPIO_NUM       12
#define Y6_GPIO_NUM       14
#define Y5_GPIO_NUM       16
#define Y4_GPIO_NUM       18
#define Y3_GPIO_NUM       17
#define Y2_GPIO_NUM       15
#define VSYNC_GPIO_NUM    38
#define HREF_GPIO_NUM     47
#define PCLK_GPIO_NUM     13

/* Captive-portal AP. The SSID is ORBIE_XXXX, where XXXX is the last two bytes
 * of the base MAC in hex — the same four characters used on the unit's label,
 * so the network name matches the sticker (e.g. ORBIE_73B4). Filled by
 * start_wifi_ap(); the initial value is only a placeholder. */
static char wifi_ssid[16] = "ORBIE_0000";

/* Per-device AP password, also derived from the MAC (e.g. "orbie-73b4-f862").
 * WPA2 is not decoration here: the captive portal collects the user's HOME
 * Wi-Fi password over plain HTTP, /ota accepts unauthenticated firmware
 * uploads, and port 81 streams live camera video. An open AP would expose all
 * three to anyone in radio range. A shared password across every unit would
 * mean one leak compromises the fleet, so this is unique per robot. */
static char wifi_pass[24] = "orbie-0000-0000";

/* Shared secret for /ota, derived from the MAC alongside the SSID. See the
 * comment above ota_request_from_ap() for what this does and does not buy. */
static char ota_token[24] = "";

/* Where push-to-talk lives. Held in NVS rather than compiled in, so moving
 * from a personal Vercel deployment to apis.orbierobot.com later is a POST,
 * not a reflash of every robot in the field. */
#define API_NVS_NS   "orbie"
#define API_NVS_KEY  "api_base"

/* The server's per-robot secret. Minted by /api/devices/register and written
 * here by the app over the LAN, so it is never compiled in and never shared
 * between units - a leak from one robot cannot be used to drive another. */
#define KEY_NVS_KEY  "device_key"
static char device_key[40] = "";
#define API_BASE_DEFAULT "https://orbie-apis.vercel.app"
static char api_base[96] = API_BASE_DEFAULT;

static void api_base_load(void)
{
    nvs_handle_t h;
    if (nvs_open(API_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(api_base);
    char tmp[96];
    if (nvs_get_str(h, API_NVS_KEY, tmp, &len) == ESP_OK && tmp[0]) {
        strlcpy(api_base, tmp, sizeof(api_base));
    }
    nvs_close(h);
}

static void device_key_load(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(API_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "device key: nvs_open failed (%s)", esp_err_to_name(err));
        return;
    }
    size_t len = sizeof(device_key);
    char tmp[40];
    err = nvs_get_str(h, KEY_NVS_KEY, tmp, &len);
    if (err == ESP_OK && tmp[0]) {
        strlcpy(device_key, tmp, sizeof(device_key));
        ESP_LOGI(TAG, "device key loaded (%d chars)", (int)strlen(device_key));
    } else {
        /* Not an error on a fresh robot - it just has not been paired yet.
         * Anything other than NOT_FOUND is worth seeing, because a key that
         * silently fails to load looks exactly like a server rejecting us. */
        ESP_LOGW(TAG, "device key: not loaded (%s)", esp_err_to_name(err));
    }
    nvs_close(h);
}

static void device_key_save(const char *key)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(API_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "device key: nvs_open rw failed (%s)", esp_err_to_name(err));
        return;
    }
    err = nvs_set_str(h, KEY_NVS_KEY, key);
    if (err == ESP_OK) err = nvs_commit(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "device key: save failed (%s)", esp_err_to_name(err));
    else               ESP_LOGI(TAG, "device key: saved to NVS");
    nvs_close(h);
}

static void api_base_save(const char *url)
{
    nvs_handle_t h;
    if (nvs_open(API_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, API_NVS_KEY, url);
    nvs_commit(h);
    nvs_close(h);
}

/* Motor pins (DRV8833) */
#define MOTOR_A_FWD  1
#define MOTOR_A_REV  2
#define MOTOR_B_FWD  4
#define MOTOR_B_REV  3
#define MOTOR_PWM_FREQ  1000
#define MOTOR_PWM_RES   8
#define MOTOR_TIMEOUT_MS 2000
#define MOTOR_MIN_PCT    40
#define MOTOR_KICK_PCT   80
#define MOTOR_KICK_MS    100
#define MOTOR_RAMP_RATE  15.0f

static volatile int motor_target_drive = 0;
static volatile int motor_target_turn = 0;
static float motor_current_drive_f = 0.0f;
static float motor_current_turn_f = 0.0f;
static bool motor_a_was_stopped = true;
static bool motor_b_was_stopped = true;
static volatile bool motors_running = false;
static int64_t last_motor_cmd_us = 0;

/* Odometry estimation (PWM+time based, no encoders) */
static volatile float odom_x = 0.0f;
static volatile float odom_y = 0.0f;
static volatile float odom_heading = 0.0f;
static uint32_t odom_trajectory[2000][3];
static int odom_traj_idx = 0;
static portMUX_TYPE odom_mux = portMUX_INITIALIZER_UNLOCKED;

#define MAX_SPEED_MPS    0.15f
#define WHEELBASE_M      0.12f
#define ODOM_UPDATE_MS   50

/* ==================== Motor Control ==================== */

static int apply_dead_zone(int abs_speed) {
    if (abs_speed <= 0) { return 0; }
    int min_pwm = (MOTOR_MIN_PCT * 255) / 100;
    return min_pwm + (abs_speed - 1) * (255 - min_pwm) / 254;
}

static void kick_start(int fwd_ch, int rev_ch, int speed) {
    int kick = (MOTOR_KICK_PCT * 255) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)fwd_ch, speed > 0 ? kick : 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)fwd_ch);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)rev_ch, speed < 0 ? kick : 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)rev_ch);
    vTaskDelay(pdMS_TO_TICKS(MOTOR_KICK_MS));
}

static void set_one_motor(int fwd_ch, int rev_ch, int speed, bool *was_stopped, bool skip_kick) {
    if (speed != 0) {
        if (*was_stopped && !skip_kick) { kick_start(fwd_ch, rev_ch, speed); }
        *was_stopped = false;
        int fwd_duty = speed > 0 ? apply_dead_zone(speed) : 0;
        int rev_duty = speed < 0 ? apply_dead_zone(-speed) : 0;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)fwd_ch, fwd_duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)fwd_ch);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)rev_ch, rev_duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)rev_ch);
    } else {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)fwd_ch, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)fwd_ch);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)rev_ch, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)rev_ch);
        *was_stopped = true;
    }
}

static void set_motors(int drive, int turn, bool skip_kick) {
    int left = drive - turn;
    int right = drive + turn;
    if (left > 255) { left = 255; }
    if (left < -255) { left = -255; }
    if (right > 255) { right = 255; }
    if (right < -255) { right = -255; }
    set_one_motor(LEDC_CHANNEL_0, LEDC_CHANNEL_1, left, &motor_a_was_stopped, skip_kick);
    set_one_motor(LEDC_CHANNEL_2, LEDC_CHANNEL_3, right, &motor_b_was_stopped, skip_kick);
    motors_running = (drive != 0 || turn != 0);
}

static void motors_stop(void) {
    motor_target_drive = 0;
    motor_target_turn = 0;
    motor_current_drive_f = 0.0f;
    motor_current_turn_f = 0.0f;
    set_motors(0, 0, false);
    motors_running = false;
    motor_a_was_stopped = true;
    motor_b_was_stopped = true;
}

static void motor_ramp_task(void *pv) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(20));
        float td = (float)motor_target_drive;
        float tt = (float)motor_target_turn;
        float dd = td - motor_current_drive_f;
        if (dd > MOTOR_RAMP_RATE) { dd = MOTOR_RAMP_RATE; }
        if (dd < -MOTOR_RAMP_RATE) { dd = -MOTOR_RAMP_RATE; }
        motor_current_drive_f += dd;
        if (fabsf(motor_current_drive_f - td) < 0.5f) { motor_current_drive_f = td; }
        float dt = tt - motor_current_turn_f;
        if (dt > MOTOR_RAMP_RATE) { dt = MOTOR_RAMP_RATE; }
        if (dt < -MOTOR_RAMP_RATE) { dt = -MOTOR_RAMP_RATE; }
        motor_current_turn_f += dt;
        if (fabsf(motor_current_turn_f - tt) < 0.5f) { motor_current_turn_f = tt; }
        set_motors((int)motor_current_drive_f, (int)motor_current_turn_f, true);
    }
}

static void init_motors(void) {
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = (ledc_timer_bit_t)MOTOR_PWM_RES,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = MOTOR_PWM_FREQ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    const int motor_pins[4] = { MOTOR_A_FWD, MOTOR_A_REV, MOTOR_B_FWD, MOTOR_B_REV };
    const ledc_channel_t motor_chs[4] = { LEDC_CHANNEL_0, LEDC_CHANNEL_1, LEDC_CHANNEL_2, LEDC_CHANNEL_3 };
    for (int i = 0; i < 4; i++) {
        ledc_channel_config_t ch = {
            .gpio_num = motor_pins[i],
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = motor_chs[i],
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
            .hpoint = 0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&ch));
    }
    motors_stop();
    xTaskCreate(motor_ramp_task, "motor_ramp", 2048, NULL, 2, NULL);
}

static void motor_safety_check(void) {
    if (motors_running && (esp_timer_get_time() - last_motor_cmd_us > MOTOR_TIMEOUT_MS * 1000)) {
        motors_stop();
    }
}

/* ==================== Web Handlers ==================== */

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, CONTROL_PAGE_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t update_page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    /* The page is only reachable from the AP, so handing it the token here is
     * no weaker than the AP gate itself - and it keeps the upload one click. */
    char tok[96];
    snprintf(tok, sizeof(tok), "<script>const OTA_TOKEN='%s';</script>", ota_token);
    httpd_resp_sendstr_chunk(req, tok);
    httpd_resp_sendstr_chunk(req,
"<!doctype html><html><head>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>OTA Update</title>"
"<style>body{background:#111;color:#eee;font-family:Arial;padding:24px}button,input{font-size:16px;margin-top:12px}</style>"
"</head><body><h1>OTA Update</h1>"
"<input id='file' type='file' accept='.bin'>"
"<br><button onclick='u()'>Upload firmware</button>"
"<pre id='st'></pre>"
"<script>async function u(){var f=document.getElementById('file').files[0];if(!f){st.textContent='Choose a .bin file first';return;}st.textContent='Uploading...';var r=await fetch('/ota',{method:'POST',headers:{'X-Orbie-Token':OTA_TOKEN},body:f});st.textContent=await r.text();}</script>"
"</body></html>"
    );
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* OTA is the highest-value target on this robot: a successful upload owns the
 * device permanently. Two independent gates guard it.
 *
 * 1. AP-only. The request must arrive on our own 192.168.4.0/24 SoftAP, so an
 *    attacker has to be in radio range. Once the robot joins the user's home
 *    Wi-Fi it is reachable from every device on that LAN, and without this
 *    check every one of them could reflash it.
 * 2. A shared token in X-Orbie-Token, which /update embeds automatically.
 *
 * The token is derived from the MAC, so it is obscurity rather than real
 * secrecy - the SSID already reveals two of its bytes and the OUI is public,
 * leaving ~16 bits. It stops accidents and casual pokes, not a determined
 * attacker. Gate 1 is the one doing the real work. Before this ships, replace
 * the token with a random per-device secret stored in NVS, or require a
 * physical button press to arm an update. */
static bool ota_request_from_ap(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) return false;

    /* MUST be sockaddr_storage, not sockaddr_in. With CONFIG_LWIP_IPV6 (the
     * default) esp_http_server listens on PF_INET6, so getsockname() hands
     * back a sockaddr_in6 and every v4 client arrives IPv4-mapped. Reading
     * that through a sockaddr_in lands on sin6_flowinfo instead of the
     * address, so the subnet test compared against zero and rejected
     * everything - including the robot's own AP. OTA could never succeed. */
    struct sockaddr_storage local = {};
    socklen_t len = sizeof(local);
    if (getsockname(fd, (struct sockaddr *)&local, &len) != 0) return false;

    uint32_t ip;
    if (local.ss_family == AF_INET6) {
        const struct sockaddr_in6 *a6 = (const struct sockaddr_in6 *)&local;
        const uint8_t *b = a6->sin6_addr.s6_addr;
        /* ::ffff:a.b.c.d - an IPv4 peer on the dual-stack socket. */
        bool v4_mapped = !memcmp(b, "\0\0\0\0\0\0\0\0\0\0\xff\xff", 12);
        if (!v4_mapped) {
            ESP_LOGW(TAG, "OTA from a real IPv6 address - not recognised as local");
            return false;
        }
        ip = ((uint32_t)b[12] << 24) | ((uint32_t)b[13] << 16) |
             ((uint32_t)b[14] << 8)  |  (uint32_t)b[15];
    } else if (local.ss_family == AF_INET) {
        ip = ntohl(((const struct sockaddr_in *)&local)->sin_addr.s_addr);
    } else {
        return false;
    }

    ESP_LOGI(TAG, "OTA request arrived on %u.%u.%u.%u",
             (unsigned)(ip >> 24), (unsigned)((ip >> 16) & 0xff),
             (unsigned)((ip >> 8) & 0xff), (unsigned)(ip & 0xff));

    /* The SoftAP subnet is always allowed. */
    if ((ip & 0xFFFFFF00u) == 0xC0A80400u) return true;

#ifdef ORBIE_OTA_ALLOW_LAN
    /* Dev builds also accept the owner's own network. The AP has to share the
     * STA's channel, which makes it unreliable to join while the robot is
     * online - and being unable to update a robot you can see is worse than
     * the risk here, on a home network, behind a token.
     *
     * NOT for shipping: it means any device on the network can reflash the
     * robot, and the token is MAC-derived and therefore weak. */
    if ((ip & 0xFF000000u) == 0x0A000000u ||      /* 10.0.0.0/8      */
        (ip & 0xFFF00000u) == 0xAC100000u ||      /* 172.16.0.0/12   */
        (ip & 0xFFFF0000u) == 0xC0A80000u) {      /* 192.168.0.0/16  */
        ESP_LOGW(TAG, "OTA from the LAN allowed (dev build)");
        return true;
    }
#endif
    return false;
}

static bool ota_token_ok(httpd_req_t *req)
{
    char given[40] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-Orbie-Token", given, sizeof(given)) != ESP_OK) {
        return false;
    }
    return strcmp(given, ota_token) == 0;
}

static esp_err_t ota_handler(httpd_req_t *req)
{
    if (!ota_request_from_ap(req)) {
        ESP_LOGW(TAG, "OTA rejected: not from the ORBIE AP");
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
                            "Firmware updates are only accepted from the robot's own Wi-Fi.");
        return ESP_FAIL;
    }
    if (!ota_token_ok(req)) {
        ESP_LOGW(TAG, "OTA rejected: bad or missing X-Orbie-Token");
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Missing or invalid update token.");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA accepted (%d bytes incoming)", req->content_len);

    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }
    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }
    char buffer[1024];
    int remaining = req->content_len;
    int timeouts = 0;
    #define OTA_MAX_TIMEOUTS 50
    while (remaining > 0) {
        int received = httpd_req_recv(req, buffer, sizeof(buffer));

        /* A timeout is not a failure. The upload is ~1.5MB and the robot is
         * doing other work while it arrives, so a socket read can come up
         * empty under load; treating that as fatal threw away the entire
         * transfer minutes in. Only a real error or a closed connection ends
         * it. */
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > OTA_MAX_TIMEOUTS) {
                ESP_LOGE(TAG, "OTA gave up after %d consecutive timeouts", timeouts);
                esp_ota_abort(ota_handle);
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload stalled");
                return ESP_FAIL;
            }
            continue;
        }
        if (received <= 0) {
            ESP_LOGE(TAG, "OTA recv failed (%d) with %d bytes to go", received, remaining);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload failed");
            return ESP_FAIL;
        }
        timeouts = 0;
        /* A dropped write must abort. Setting the boot partition after a
         * partial image bricks the robot on the next reboot. */
        err = esp_ota_write(ota_handle, buffer, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Flash write failed");
            return ESP_FAIL;
        }
        remaining -= received;
    }
    /* esp_ota_end validates the image; a truncated or corrupt upload fails here. */
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid firmware image");
        return ESP_FAIL;
    }
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not switch partition");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA written and validated, rebooting");
    httpd_resp_sendstr(req, "Update successful. Rebooting...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

/* Stream on a SEPARATE HTTP server (port 81) so it never blocks control requests on port 80 */
static esp_err_t stream_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");
    while (true) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) { break; }

        /* Every exit from here MUST return the buffer. There are only fb_count
         * of them, so a client that disconnects mid-frame used to strand one
         * permanently - two of those and esp_camera_fb_get() returns NULL for
         * good, which shows up as /capture failing 500 forever until reboot. */
        char boundary[] = "--frame\r\nContent-Type: image/jpeg\r\n\r\n";
        bool sent = httpd_resp_send_chunk(req, boundary, strlen(boundary)) == ESP_OK
                 && httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len) == ESP_OK
                 && httpd_resp_send_chunk(req, "\r\n", 2) == ESP_OK;
        esp_camera_fb_return(fb);

        if (!sent) break;
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    return ESP_OK;
}

static void init_laser() {
    gpio_reset_pin(LASER_GPIO);
    gpio_set_direction(LASER_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LASER_GPIO, 1); /* active-low: HIGH = OFF */
}

/* Speaker (MAX98357 I2S): DOUT=44, BCLK=7, LRC=8 */
static i2s_chan_handle_t speaker_handle = NULL;
static bool speaker_ok = false;

/* The MAX98357 amplifies whatever is on its input, and an I2S channel that is
 * enabled but idle still clocks near-silence at it - which the amp faithfully
 * turns into a constant hiss. So the channel is only enabled while something
 * is actually playing, and disabled the moment it stops.
 *
 * The mutex matters because the boot greeting, a beep and a /say can all
 * arrive at once: without it, one could disable the channel while another is
 * mid-write, which surfaces as a truncated clip or an I2S error. */
static SemaphoreHandle_t audio_mux = NULL;

static bool speaker_begin(void)
{
    if (!speaker_ok || !speaker_handle || !audio_mux) return false;
    if (xSemaphoreTake(audio_mux, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGW(TAG, "audio busy - skipping playback");
        return false;
    }
    if (i2s_channel_enable(speaker_handle) != ESP_OK) {
        xSemaphoreGive(audio_mux);
        return false;
    }
    return true;
}

static void speaker_end(void)
{
    if (!speaker_handle || !audio_mux) return;
    i2s_channel_disable(speaker_handle);   /* stops the clocks: silence, not hiss */
    xSemaphoreGive(audio_mux);
}

static void init_speaker(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    if (i2s_new_channel(&chan_cfg, &speaker_handle, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "Speaker channel creation failed");
        return;
    }
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BOOT_PCM_SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)7,
            .ws = (gpio_num_t)8,
            .dout = (gpio_num_t)44,
            .din = I2S_GPIO_UNUSED,
        },
    };
    if (i2s_channel_init_std_mode(speaker_handle, &std_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "Speaker I2S init failed");
        i2s_del_channel(speaker_handle);
        speaker_handle = NULL;
        return;
    }
    audio_mux = xSemaphoreCreateMutex();
    speaker_ok = (audio_mux != NULL);
    /* Left DISABLED on purpose - see speaker_begin(). */
    ESP_LOGI(TAG, "Speaker initialized (GPIO44/7/8, %dHz, idle-silent)",
             BOOT_PCM_SAMPLE_RATE);
}

/* Playback volume, 0-100. The MAX98357 has no gain control we can reach, so
 * this scales the samples before they leave the MCU. The greeting is mastered
 * to -1 dBFS so it carries at a demo, which is far too much for a desk.
 *
 * Persisted to NVS: the boot greeting plays before any HTTP request can arrive,
 * so a runtime-only setting would mean reflashing to change how loud the robot
 * greets you. Set it once with the slider and it sticks. */
#define VOLUME_NVS_NS  "orbie"
#define VOLUME_NVS_KEY "volume"
static volatile int volume_pct = 8;

static void volume_load(void)
{
    nvs_handle_t h;
    if (nvs_open(VOLUME_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    int32_t v = 0;
    if (nvs_get_i32(h, VOLUME_NVS_KEY, &v) == ESP_OK && v >= 0 && v <= 100) {
        volume_pct = (int)v;
    }
    nvs_close(h);
}

static void volume_save(int pct)
{
    nvs_handle_t h;
    if (nvs_open(VOLUME_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, VOLUME_NVS_KEY, (int32_t)pct);
    nvs_commit(h);
    nvs_close(h);
}

/* Scale into a small stack buffer rather than one allocation the size of the
 * clip: the greeting alone is 247KB, and there is no reason to hold a second
 * copy of it in RAM. */
static size_t i2s_write_scaled(const int16_t *src, size_t samples)
{
    const size_t CHUNK = 512;
    int16_t buf[CHUNK];
    size_t written = 0, total = 0;

    for (size_t off = 0; off < samples; off += CHUNK) {
        size_t n = (samples - off) < CHUNK ? (samples - off) : CHUNK;
        int vol = volume_pct;              /* sample once per chunk */
        for (size_t i = 0; i < n; i++) {
            buf[i] = (int16_t)(((int32_t)src[off + i] * vol) / 100);
        }
        if (i2s_channel_write(speaker_handle, buf, n * sizeof(int16_t), &written,
                              portMAX_DELAY) != ESP_OK) {
            break;
        }
        total += written;
    }
    return total;
}

static void play_boot_sound(void)
{
    if (!speaker_begin()) return;
    size_t wrote = i2s_write_scaled((const int16_t *)boot_pcm_data, BOOT_PCM_NUM_SAMPLES);
    speaker_end();
    ESP_LOGI(TAG, "Boot sound played (%u of %u bytes at %d%% volume)",
             (unsigned)wrote, (unsigned)BOOT_PCM_NUM_BYTES, volume_pct);
}

/**
 * Free the I2C bus if a slave is still holding SDA down.
 *
 * A soft reset - esptool's USB reset, a panic, an OTA reboot - restarts the
 * ESP32 mid-transaction. The slave it was talking to never sees the rest of
 * the clock, so it keeps driving SDA low waiting for it. SDA stuck low means
 * no master can issue a START, and EVERY address NACKs: the LED matrix stops
 * responding and the face never appears, while a power cycle "fixes" it.
 *
 * The recovery is the one in the I2C spec: bit-bang up to nine SCL pulses with
 * SDA released, which walks the slave through the byte it was stuck in, then a
 * manual STOP. Must run before the driver claims the pins.
 */
static void i2c_bus_recover(void)
{
    const gpio_num_t sda = GPIO_NUM_5, scl = GPIO_NUM_6;

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << sda) | (1ULL << scl),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,     /* open-drain: we only ever pull low */
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(sda, 1);
    gpio_set_level(scl, 1);
    esp_rom_delay_us(10);

    if (gpio_get_level(sda)) return;           /* bus is idle - nothing to do */

    ESP_LOGW(TAG, "I2C SDA held low - clocking the bus free");
    for (int i = 0; i < 9 && !gpio_get_level(sda); i++) {
        gpio_set_level(scl, 0);
        esp_rom_delay_us(5);
        gpio_set_level(scl, 1);
        esp_rom_delay_us(5);
    }

    /* STOP: SDA low->high while SCL is high. */
    gpio_set_level(sda, 0);
    esp_rom_delay_us(5);
    gpio_set_level(scl, 1);
    esp_rom_delay_us(5);
    gpio_set_level(sda, 1);
    esp_rom_delay_us(5);

    ESP_LOGW(TAG, "I2C recovery %s", gpio_get_level(sda) ? "succeeded" : "FAILED - SDA still low");
}

static void init_i2c_bus(void)
{
    i2c_bus_recover();

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = GPIO_NUM_5,
        .scl_io_num = GPIO_NUM_6,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = 1 },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus));

    /* Scan once at boot. Knowing WHICH addresses answer turns "the face is
     * broken" into a specific question: a missing 0x50 with an unexpected
     * address present means the ADDR pins are floating, and nothing present
     * means the chip is unpowered or unsoldered. */
    {
        char found[128];
        int n = 0;
        found[0] = 0;
        for (uint8_t addr = 0x08; addr < 0x78; addr++) {
            if (i2c_master_probe(i2c_bus, addr, 50) == ESP_OK) {
                n += snprintf(found + n, sizeof(found) - n, " 0x%02x", addr);
                if (n >= (int)sizeof(found) - 8) break;
            }
        }
        ESP_LOGW(TAG, "I2C scan:%s", n ? found : " (nothing responded)");
    }
    ESP_LOGI(TAG, "Shared I2C bus initialized (SDA=GPIO5, SCL=GPIO6)");
}

/* MLX90614: IR temperature sensor on same I2C bus (addr 0x5A)
   Registers: 0x06=ambient temp, 0x07=object temp
   Each returns 2 bytes (little-endian). Value = data*0.02 - 273.15 */
static void init_temp_sensor(void)
{
    /* Try to wake MLX90614 by sending its address repeatedly */
    uint8_t wake_addr[] = { 0x5A, 0x5B, 0x5C, 0x5D };
    ESP_LOGI(TAG, "Probing I2C bus for MLX90614...");

    for (int i = 0; i < 4; i++) {
        uint8_t addr = wake_addr[i];
        esp_err_t err = i2c_master_probe(i2c_bus, addr, 100);
        if (err == ESP_OK) {
            /* Device ACKed its address. Try reading temp register */
            i2c_device_config_t dev_cfg = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address = addr,
                .scl_speed_hz = 100000,
            };
            esp_err_t add_err = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &temp_dev);
            if (add_err != ESP_OK) continue;

            uint8_t reg = 0x06;
            uint8_t buf[3] = {0};
            err = i2c_master_transmit_receive(temp_dev, &reg, 1, buf, 3, 100);
            if (err == ESP_OK) {
                int16_t raw = (int16_t)(buf[0] | (buf[1] << 8));
                float temp = (float)raw * 0.02f - 273.15f;
                if (temp > -50 && temp < 150) {
                    temp_ok = true;
                    ESP_LOGI(TAG, "MLX90614 found at 0x%02x (temp=%.1fC)", addr, temp);
                    return;
                }
            } else {
                /* Try with PEC byte */
                reg = 0x06;
                uint8_t cmd = 0x06;
                uint8_t wbuf[1] = {cmd};
                err = i2c_master_transmit(temp_dev, wbuf, 1, 100);
                if (err == ESP_OK) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    err = i2c_master_receive(temp_dev, buf, 3, 100);
                    if (err == ESP_OK) {
                        int16_t raw = (int16_t)(buf[0] | (buf[1] << 8));
                        float temp = (float)raw * 0.02f - 273.15f;
                        if (temp > -50 && temp < 150) {
                            temp_ok = true;
                            ESP_LOGI(TAG, "MLX90614 found at 0x%02x via split tx/rx (temp=%.1fC)", addr, temp);
                            return;
                        }
                    }
                }
            }
            i2c_master_bus_rm_device(temp_dev);
            temp_dev = NULL;
        }
    }

    ESP_LOGW(TAG, "No MLX90614 found on I2C bus (probed 0x5A-0x5D)");
}

static void read_temp_sensor(void)
{
    if (!i2c_lock_take(100)) return;   /* skip this sample, try again in 200ms */
    if (!temp_ok || !temp_dev) return;
    uint8_t reg;
    uint8_t buf[2];

    /* Read ambient temperature (register 0x06) */
    reg = 0x06;
    if (i2c_master_transmit_receive(temp_dev, &reg, 1, buf, 2, 100) == ESP_OK) {
        int16_t raw = (int16_t)(buf[0] | (buf[1] << 8));
        temp_ambient = (float)raw * 0.02f - 273.15f;
    }

    /* Read object temperature (register 0x07) */
    reg = 0x07;
    if (i2c_master_transmit_receive(temp_dev, &reg, 1, buf, 2, 100) == ESP_OK) {
        int16_t raw = (int16_t)(buf[0] | (buf[1] << 8));
        temp_object = (float)raw * 0.02f - 273.15f;
    }
    i2c_lock_give();
}

/* Head-tilt servo travel. See head_handler() for why these are conservative. */
#define HEAD_US_MIN    1700
#define HEAD_US_MAX    2500
#define HEAD_US_CENTER 2425

/* The head servo is DISABLED by default.
 *
 * Left holding a position it cannot reach, a hobby servo buzzes continuously,
 * draws heavy stall current and will eventually strip its gears or brown out
 * the board. Orbie was doing exactly that - an audible hiss with the head at
 * its boot position - so nothing drives the pin unless it is switched on.
 *
 * Disabled means no pulses at all, which lets the servo go limp rather than
 * fighting; a servo given no signal holds nothing and draws nothing.
 * Re-enable at runtime with /head?enable=1 once the mechanism is trusted. */
static volatile bool head_enabled = false;
static volatile int  head_us = HEAD_US_CENTER;

static void head_disable(void)
{
    /* Stop the PWM and park the pin low: no pulse train, no holding torque. */
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_5, 0);
    head_enabled = false;
    ESP_LOGI(TAG, "Head servo DISABLED (no drive signal)");
}

static void head_set_us(int us)
{
    if (us < HEAD_US_MIN) us = HEAD_US_MIN;
    if (us > HEAD_US_MAX) us = HEAD_US_MAX;
    head_us = us;
    if (!head_enabled) return;   /* remember the target, drive nothing */
    /* 50Hz = 20000us period, 14-bit = 16384 steps. 2425us -> 1987, matching
     * the constant the original firmware hard-coded. */
    uint32_t duty = (uint32_t)(((int64_t)us * 16384) / 20000);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_5, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_5);
}

/* Center the head-tilt servo (GPIO43) to stop it from jittering */
static void init_servo() {
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_14_BIT,
        .timer_num = LEDC_TIMER_2,
        .freq_hz = 50,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);
    ledc_channel_config_t ch = {
        .gpio_num = 43,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_5,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_2,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&ch);
    /* Configured but not driven. See head_disable() above. */
    head_disable();
}

/* Head tilt, GPIO43 / LEDC channel 5, 50Hz with 14-bit resolution.
 *
 * 2425us is the only position this hardware is known to tolerate - it is what
 * the original firmware parks at on boot. That sits close to the usual 2500us
 * upper limit for a hobby servo, which suggests nearly all the travel runs
 * downward from there, so the window below is deliberately cautious: it is a
 * guess at the mechanical limits, not a measurement. Widen HEAD_US_MIN only
 * after watching the head actually reach it - a servo driven past its stop
 * stalls, draws heavily and will strip its gears or brown out the board. */
static esp_err_t head_handler(httpd_req_t *req)
{
    char query[64], val[16];
    int us = head_us;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "enable", val, sizeof(val)) == ESP_OK) {
            if (atoi(val)) {
                head_enabled = true;
                ESP_LOGW(TAG, "Head servo ENABLED - watch for buzzing at the limits");
                head_set_us(head_us);
            } else {
                head_disable();
            }
        }
        if (httpd_query_key_value(query, "us", val, sizeof(val)) == ESP_OK) {
            us = atoi(val);
        } else if (httpd_query_key_value(query, "pos", val, sizeof(val)) == ESP_OK) {
            int pos = atoi(val);
            if (pos < 0)   pos = 0;
            if (pos > 100) pos = 100;
            us = HEAD_US_MIN + (HEAD_US_MAX - HEAD_US_MIN) * pos / 100;
        }
    }
    head_set_us(us);   /* clamps */
    ESP_LOGI(TAG, "/head -> %dus (duty %u, range %d-%d)", head_us,
             (unsigned)(((int64_t)head_us * 16384) / 20000), HEAD_US_MIN, HEAD_US_MAX);

    char out[96];
    snprintf(out, sizeof(out), "{\"us\":%d,\"pos\":%d,\"min\":%d,\"max\":%d,\"enabled\":%s}",
             head_us,
             (head_us - HEAD_US_MIN) * 100 / (HEAD_US_MAX - HEAD_US_MIN),
             HEAD_US_MIN, HEAD_US_MAX, head_enabled ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

static esp_err_t motor_handler(httpd_req_t *req)
{
    char qbuf[64];
    int drive = 0, turn = 0;
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        char val[16];
        if (httpd_query_key_value(qbuf, "drive", val, sizeof(val)) == ESP_OK) {
            drive = atoi(val);
            if (drive > 255) { drive = 255; }
            if (drive < -255) { drive = -255; }
        }
        if (httpd_query_key_value(qbuf, "turn", val, sizeof(val)) == ESP_OK) {
            turn = atoi(val);
            if (turn > 255) { turn = 255; }
            if (turn < -255) { turn = -255; }
        }
    }
    motor_target_drive = drive;
    motor_target_turn = turn;
    last_motor_cmd_us = esp_timer_get_time();
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t laser_handler(httpd_req_t *req)
{
    g_laser_auto = false;
    g_laser_enabled = !g_laser_enabled;
    gpio_set_level(LASER_GPIO, g_laser_enabled ? 0 : 1); /* active-low */
    httpd_resp_sendstr(req, g_laser_enabled ? "LASER_ON" : "LASER_OFF");
    return ESP_OK;
}

/* Two short rising chirps, synthesised on the fly rather than stored: a beep
 * is ~0.1s of a sine wave, and a table for it would cost flash for nothing.
 * Shaped with a raised-cosine envelope because a square-edged tone makes the
 * little speaker click audibly at both ends. */
static void play_beep(void)
{
    if (!speaker_begin()) return;
    const int   sr        = BOOT_PCM_SAMPLE_RATE;   /* 16000 */
    const int   tone_ms   = 90;
    const int   gap_ms    = 60;
    const float freqs[2]  = { 880.0f, 1320.0f };    /* A5 then E6 */
    const int   n         = (sr * tone_ms) / 1000;
    const int   gap_n     = (sr * gap_ms) / 1000;
    const float amplitude = 9000.0f * (volume_pct / 100.0f);  /* short of clipping */

    int16_t *buf = (int16_t *)malloc(n * sizeof(int16_t));
    if (!buf) { speaker_end(); return; }

    size_t written = 0, pushed = 0;
    for (int t = 0; t < 2; t++) {
        for (int i = 0; i < n; i++) {
            float env = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (n - 1)));
            buf[i] = (int16_t)(amplitude * env *
                               sinf(2.0f * (float)M_PI * freqs[t] * i / sr));
        }
        esp_err_t werr = i2s_channel_write(speaker_handle, buf, n * sizeof(int16_t),
                                           &written, pdMS_TO_TICKS(500));
        pushed += written;
        if (werr != ESP_OK) {
            ESP_LOGW(TAG, "beep: i2s write failed: %s", esp_err_to_name(werr));
        }
        if (t == 0) {
            memset(buf, 0, (gap_n < n ? gap_n : n) * sizeof(int16_t));
            i2s_channel_write(speaker_handle, buf,
                              (gap_n < n ? gap_n : n) * sizeof(int16_t),
                              &written, pdMS_TO_TICKS(200));
        }
    }
    free(buf);
    speaker_end();
    /* If this reports the expected byte count and nothing is audible, the
     * fault is downstream of the MCU - amplifier, its enable pin, or the
     * speaker itself - not in this code. */
    ESP_LOGI(TAG, "beep: %u bytes reached I2S", (unsigned)pushed);
}

/* Single JPEG frame, for handing the scene to a vision model. The MJPEG
 * stream on :81 is a multipart response and awkward to grab one frame from. */
static esp_err_t capture_handler(httpd_req_t *req)
{
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        /* Almost always means every frame buffer is checked out, not that the
         * sensor died - worth saying so, because the two need different fixes. */
        ESP_LOGE(TAG, "capture failed: no frame buffer available");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Camera capture failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=orbie.jpg");
    esp_err_t r = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    return r;
}

/* POST raw PCM - 16kHz, 16-bit, mono, same format as the boot greeting - and
 * it plays on the speaker. Deliberately dumb: the MCU cannot decode mp3 or
 * opus, so whatever produces Orbie's voice converts to PCM first and the robot
 * just streams bytes to I2S. Chunked rather than buffered whole, so a long
 * answer does not need to fit in RAM. */
#define SAY_MAX_BYTES (30 * 16000 * 2)   /* 30s ceiling */

static esp_err_t say_handler(httpd_req_t *req)
{
    if (!speaker_begin()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Speaker unavailable or busy");
        return ESP_FAIL;
    }
    if (req->content_len <= 0 || req->content_len > SAY_MAX_BYTES) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad length (max 30s of 16k mono PCM)");
        return ESP_FAIL;
    }

    const int CHUNK = 2048;
    uint8_t *buf = (uint8_t *)malloc(CHUNK);
    if (!buf) {
        speaker_end();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    size_t written = 0, total = 0;
    while (remaining > 0) {
        int want = remaining < CHUNK ? remaining : CHUNK;
        int got  = httpd_req_recv(req, (char *)buf, want);
        if (got <= 0) {
            free(buf);
            speaker_end();
            ESP_LOGW(TAG, "/say upload aborted after %u bytes", (unsigned)total);
            return ESP_FAIL;   /* socket is already gone; no response to send */
        }
        /* got is a byte count; the payload is 16-bit samples. An odd tail
         * would split a sample, so scale only whole ones. */
        int16_t *s16 = (int16_t *)buf;
        int vol = volume_pct;
        for (int i = 0; i < got / 2; i++) {
            s16[i] = (int16_t)(((int32_t)s16[i] * vol) / 100);
        }
        i2s_channel_write(speaker_handle, buf, got, &written, pdMS_TO_TICKS(2000));
        remaining -= got;
        total     += got;
    }
    free(buf);
    speaker_end();
    ESP_LOGI(TAG, "/say played %u bytes (%.1fs)", (unsigned)total,
             total / (float)(BOOT_PCM_SAMPLE_RATE * 2));
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/* ==================== Push-to-talk glue ====================
 *
 * voice_link owns the network side; these adapt it to hardware that already
 * exists. Playback reuses the same volume-scaled I2S path as the greeting, so
 * an answer obeys the volume slider like everything else. */

static void ptt_play_pcm(const uint8_t *pcm, size_t len, bool first)
{
    /* The channel is enabled on the first chunk and left open for the rest of
     * the clip: toggling it per chunk would click between every 1KB. */
    if (first && !speaker_begin()) return;
    if (!speaker_ok) return;
    i2s_write_scaled((const int16_t *)pcm, len / 2);
}

static void ptt_play_done(void)
{
    speaker_end();
}

static camera_fb_t *ptt_fb = NULL;

static const uint8_t *ptt_grab_frame(size_t *len_out)
{
    ptt_fb = esp_camera_fb_get();
    if (!ptt_fb) { *len_out = 0; return NULL; }
    *len_out = ptt_fb->len;
    return ptt_fb->buf;
}

static void ptt_release_frame(void)
{
    if (ptt_fb) { esp_camera_fb_return(ptt_fb); ptt_fb = NULL; }
}

/* ==================== Self-test endpoints ====================
 *
 * Each runs in its own task and returns immediately. Doing them inline would
 * block the HTTP worker for seconds - the eye reel alone is ~7s - and the
 * browser would sit on a spinner or time out mid-test.
 *
 * They exist so the panel can exercise every subsystem without a second
 * device: on a robot with no speaker, or no distance sensor, you want to find
 * out by pressing a button, not by inferring it from a demo that fell flat. */

static void test_voice_task(void *pv)
{
    /* The first ~2.5s of the greeting, at whatever volume is set. Using the
     * real greeting rather than a tone is the point: this answers "how loud
     * will it be when it introduces itself", which a beep does not. */
    if (speaker_begin()) {
        size_t samples = BOOT_PCM_SAMPLE_RATE * 5 / 2;
        if (samples > BOOT_PCM_NUM_SAMPLES) samples = BOOT_PCM_NUM_SAMPLES;
        i2s_write_scaled((const int16_t *)boot_pcm_data, samples);
        speaker_end();
        ESP_LOGI(TAG, "voice test played at %d%%", volume_pct);
    }
    vTaskDelete(NULL);
}

static void test_eyes_task(void *pv)
{
    display_play_demo_sequence();
    vTaskDelete(NULL);
}

static void test_head_task(void *pv)
{
    /* Sweep the full allowed travel and come back to centre, slowly enough to
     * watch. If the head does not move, the servo or its wiring is the fault -
     * the log will still show the pulse widths going out. */
    const int steps = 12;
    for (int i = 0; i <= steps; i++) {
        head_set_us(HEAD_US_MIN + (HEAD_US_MAX - HEAD_US_MIN) * i / steps);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    for (int i = steps; i >= 0; i--) {
        head_set_us(HEAD_US_MIN + (HEAD_US_MAX - HEAD_US_MIN) * i / steps);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    head_set_us(HEAD_US_CENTER);
    ESP_LOGI(TAG, "head sweep complete, back to %dus", HEAD_US_CENTER);
    vTaskDelete(NULL);
}

static void test_motors_task(void *pv)
{
    /* Short, gentle nudges. Long enough to see and hear, short enough not to
     * drive the robot off the desk. */
    motor_target_drive = 90;  motor_target_turn = 0;
    last_motor_cmd_us = esp_timer_get_time();
    vTaskDelay(pdMS_TO_TICKS(450));
    motor_target_drive = -90;
    last_motor_cmd_us = esp_timer_get_time();
    vTaskDelay(pdMS_TO_TICKS(450));
    motors_stop();
    ESP_LOGI(TAG, "motor test complete");
    vTaskDelete(NULL);
}

static char expr_pending[16] = "";

static void expr_task(void *pv)
{
    display_play_named(expr_pending);
    /* Animations leave the face wherever they ended; settle it. */
    if (strcmp(expr_pending, "centre") && strcmp(expr_pending, "center") &&
        strcmp(expr_pending, "up") && strcmp(expr_pending, "down") &&
        strcmp(expr_pending, "left") && strcmp(expr_pending, "right")) {
        display_look_direction(EYE_CENTER);
    }
    vTaskDelete(NULL);
}

static esp_err_t expr_handler(httpd_req_t *req)
{
    char query[48], what[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "show", what, sizeof(what));
    }
    if (!what[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "show is required");
        return ESP_FAIL;
    }
    strlcpy(expr_pending, what, sizeof(expr_pending));
    /* In a task: the reel is several seconds and would block the HTTP worker. */
    xTaskCreate(expr_task, "expr", 4096, NULL, 4, NULL);

    char out[64];
    snprintf(out, sizeof(out), "{\"ok\":true,\"show\":\"%s\"}", what);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

static esp_err_t test_handler(httpd_req_t *req)
{
    char query[48], what[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "run", what, sizeof(what));
    }

    const char *msg = "unknown test";
    if (!strcmp(what, "voice")) {
        if (!speaker_ok) { msg = "speaker not initialised"; }
        else { xTaskCreate(test_voice_task, "t_voice", 4096, NULL, 4, NULL);
               msg = "playing 2.5s of the greeting"; }
    } else if (!strcmp(what, "eyes")) {
        xTaskCreate(test_eyes_task, "t_eyes", 4096, NULL, 4, NULL);
        msg = "heart, star, loader, rainbow";
    } else if (!strcmp(what, "head")) {
        if (!head_enabled) {
            msg = "head is disabled - enable it first (/head?enable=1)";
        } else {
            xTaskCreate(test_head_task, "t_head", 3072, NULL, 4, NULL);
            msg = "sweeping the head";
        }
    } else if (!strcmp(what, "motors")) {
        xTaskCreate(test_motors_task, "t_motor", 3072, NULL, 4, NULL);
        msg = "forward then back";
    } else if (!strcmp(what, "beep")) {
        play_beep();
        msg = speaker_ok ? "beeped" : "speaker not initialised";
    }

    char out[96];
    snprintf(out, sizeof(out), "{\"ok\":true,\"msg\":\"%s\"}", msg);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

static esp_err_t whoami_handler(httpd_req_t *req)
{
    char query[160], val[96];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "api", val, sizeof(val)) == ESP_OK && val[0]) {
        strlcpy(api_base, val, sizeof(api_base));
        api_base_save(api_base);
        ESP_LOGI(TAG, "API base set to %s", api_base);
    }
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "key", val, sizeof(val)) == ESP_OK && val[0]) {
        strlcpy(device_key, val, sizeof(device_key));
        device_key_save(device_key);
        /* Never log the key itself - this console is on USB and in scrollback. */
        ESP_LOGI(TAG, "device key stored (%d chars)", (int)strlen(device_key));
        voice_link_set_key(device_key);
    }

    const esp_app_desc_t *desc = esp_app_get_description();
    char out[320];
    snprintf(out, sizeof(out),
             "{\"robot\":\"%s\",\"ip\":\"%s\",\"api\":\"%s\",\"version\":\"%s\","
             "\"built\":\"%s %s\",\"registered\":%s}",
             wifi_ssid, wifi_portal_ip(), api_base,
             desc->version, desc->date, desc->time,
             device_key[0] ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

static esp_err_t volume_handler(httpd_req_t *req)
{
    char query[48], val[12];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "pct", val, sizeof(val)) == ESP_OK) {
        int v = atoi(val);
        if (v < 0)   v = 0;
        if (v > 100) v = 100;
        volume_pct = v;
        volume_save(v);
        ESP_LOGI(TAG, "volume -> %d%% (saved)", volume_pct);
    }
    char out[32];
    snprintf(out, sizeof(out), "{\"pct\":%d}", volume_pct);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

static esp_err_t beep_handler(httpd_req_t *req)
{
    play_beep();
    httpd_resp_sendstr(req, speaker_ok ? "BEEP" : "BEEP (speaker unavailable)");
    return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "distance", g_distance_mm);
        cJSON_AddBoolToObject(root, "laser", g_laser_enabled);
        cJSON_AddNumberToObject(root, "drive", motor_target_drive);
        cJSON_AddNumberToObject(root, "turn", motor_target_turn);
        cJSON_AddNumberToObject(root, "temp_ambient", temp_ambient);
        cJSON_AddNumberToObject(root, "temp_object", temp_object);
        cJSON_AddStringToObject(root, "wifi_ssid", wifi_portal_ssid());
        cJSON_AddStringToObject(root, "wifi_ip", wifi_portal_ip());
    const char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t res = httpd_resp_sendstr(req, json);
    free((void *)json);
    cJSON_Delete(root);
    return res;
}

/* ==================== Odometry Handlers ==================== */

static esp_err_t pose_handler(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    taskENTER_CRITICAL(&odom_mux);
    cJSON_AddNumberToObject(root, "x", odom_x);
    cJSON_AddNumberToObject(root, "y", odom_y);
    cJSON_AddNumberToObject(root, "heading", odom_heading);
    taskEXIT_CRITICAL(&odom_mux);
    cJSON_AddNumberToObject(root, "drive", motor_target_drive);
    cJSON_AddNumberToObject(root, "turn", motor_target_turn);
    const char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t res = httpd_resp_sendstr(req, json);
    free((void *)json);
    cJSON_Delete(root);
    return res;
}

static esp_err_t trajectory_handler(httpd_req_t *req) {
    char buf[32768];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "{\"points\":[");
    taskENTER_CRITICAL(&odom_mux);
    int count = 0;
    for (int i = 0; i < 2000; i++) {
        int idx = (odom_traj_idx - 1 - i + 2000) % 2000;
        uint32_t x = odom_trajectory[idx][0];
        uint32_t y = odom_trajectory[idx][1];
        if (x == 0 && y == 0 && i > 10) break;
        if (count > 0) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "[%lu,%lu]", (unsigned long)x, (unsigned long)y);
        count++;
        if (pos > sizeof(buf) - 200) break;
    }
    taskEXIT_CRITICAL(&odom_mux);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t odometry_reset_handler(httpd_req_t *req) {
    (void)req;
    taskENTER_CRITICAL(&odom_mux);
    odom_x = 0; odom_y = 0; odom_heading = 0;
    memset(odom_trajectory, 0, sizeof(odom_trajectory));
    odom_traj_idx = 0;
    taskEXIT_CRITICAL(&odom_mux);
    httpd_resp_sendstr(req, "{\"success\":true}");
    return ESP_OK;
}

/* ==================== Odometry Task ==================== */

static void odometry_task(void *pv) {
    (void)pv;
    int64_t last_us = esp_timer_get_time();
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(ODOM_UPDATE_MS));
        int64_t now_us = esp_timer_get_time();
        float dt = (float)(now_us - last_us) / 1000000.0f;
        last_us = now_us;

        float drive = motor_current_drive_f;
        float turn = motor_current_turn_f;

        float speed = (fabsf(drive) / 255.0f) * MAX_SPEED_MPS;
        if (fabsf(drive) < 5) speed = 0;
        if (drive < 0) speed = -speed;

        float turn_rate = (turn / 255.0f) * (MAX_SPEED_MPS / WHEELBASE_M);
        float vx = speed * cosf(odom_heading);
        float vy = speed * sinf(odom_heading);

        taskENTER_CRITICAL(&odom_mux);
        odom_x += vx * dt;
        odom_y += vy * dt;
        odom_heading += turn_rate * dt;
        while (odom_heading > (float)M_PI) odom_heading -= 2.0f * (float)M_PI;
        while (odom_heading < (float)-M_PI) odom_heading += 2.0f * (float)M_PI;

        static int64_t last_log_us = 0;
        if (now_us - last_log_us > 100000) {
            last_log_us = now_us;
            odom_trajectory[odom_traj_idx][0] = (uint32_t)(odom_x * 1000);
            odom_trajectory[odom_traj_idx][1] = (uint32_t)(odom_y * 1000);
            odom_trajectory[odom_traj_idx][2] = (uint32_t)(odom_heading * 180.0f / (float)M_PI * 100.0f);
            odom_traj_idx = (odom_traj_idx + 1) % 2000;
        }
        taskEXIT_CRITICAL(&odom_mux);
    }
}

static void start_camera(void)
{
    camera_config_t config = {
        .pin_pwdn = PWDN_GPIO_NUM, .pin_reset = RESET_GPIO_NUM,
        .pin_xclk = XCLK_GPIO_NUM, .pin_sccb_sda = SIOD_GPIO_NUM,
        .pin_sccb_scl = SIOC_GPIO_NUM, .pin_d7 = Y9_GPIO_NUM,
        .pin_d6 = Y8_GPIO_NUM, .pin_d5 = Y7_GPIO_NUM,
        .pin_d4 = Y6_GPIO_NUM, .pin_d3 = Y5_GPIO_NUM,
        .pin_d2 = Y4_GPIO_NUM, .pin_d1 = Y3_GPIO_NUM,
        .pin_d0 = Y2_GPIO_NUM, .pin_vsync = VSYNC_GPIO_NUM,
        .pin_href = HREF_GPIO_NUM, .pin_pclk = PCLK_GPIO_NUM,
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_1, .ledc_channel = LEDC_CHANNEL_4,
        .pixel_format = PIXFORMAT_JPEG, .frame_size = FRAMESIZE_QVGA,
        .jpeg_quality = 12, .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM, .grab_mode = CAMERA_GRAB_LATEST,
    };
    if (esp_psram_get_size() > 0) { config.frame_size = FRAMESIZE_VGA; }
    ESP_ERROR_CHECK(esp_camera_init(&config));

    /* The OV3660 is mounted upside down in the shell, so the sensor is rotated
     * 180 degrees relative to the world. Flipping both axes in the sensor is
     * free - it changes the readout order, costing no CPU and no latency,
     * unlike rotating frames in software or with a CSS transform in the UI. */
    sensor_t *cam = esp_camera_sensor_get();
    if (cam) {
        cam->set_vflip(cam, 1);
        cam->set_hmirror(cam, 1);
        ESP_LOGI(TAG, "Camera orientation: rotated 180 (vflip + hmirror)");
    } else {
        ESP_LOGW(TAG, "Camera sensor handle unavailable - image will be upside down");
    }

    ESP_LOGI(TAG, "Camera ready");
}

static void start_wifi_ap(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    /* Derive ORBIE_XXXX from the base MAC (ESP_MAC_WIFI_STA is the efuse MAC,
     * the one esptool reports — the SoftAP MAC is that plus one and would not
     * match the label on the robot). */
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(wifi_ssid, sizeof(wifi_ssid), "ORBIE_%02X%02X", mac[4], mac[5]);
    snprintf(wifi_pass, sizeof(wifi_pass), "orbie-%02x%02x-%02x%02x",
             mac[4], mac[5], mac[2], mac[3]);
    snprintf(ota_token, sizeof(ota_token), "%02x%02x%02x%02x",
             mac[2], mac[3], mac[4], mac[5]);

    wifi_config_t wifi_config = {};
    strcpy((char *)wifi_config.ap.ssid, wifi_ssid);
    strcpy((char *)wifi_config.ap.password, wifi_pass);
    wifi_config.ap.ssid_len = strlen(wifi_ssid);
    wifi_config.ap.channel = 1;
    wifi_config.ap.max_connection = 4;
#ifdef ORBIE_OPEN_AP
    /* Dev convenience: no password, so phones and laptops rejoin instantly.
     * SAFE for bench work, with one exception - the captive portal posts the
     * user's HOME Wi-Fi password over plain HTTP, and an open AP leaves that
     * readable to anyone sniffing in radio range. Provision a throwaway SSID
     * or a phone hotspot while open; build WITHOUT this flag before entering a
     * real home network, and for anything that leaves the bench. */
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    wifi_config.ap.password[0] = '\0';
#else
    wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
#endif
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    #ifdef ORBIE_OPEN_AP
    ESP_LOGW(TAG, "Wi-Fi AP: %s (OPEN - dev build, no password)", wifi_ssid);
#else
    ESP_LOGI(TAG, "Wi-Fi AP: %s / %s", wifi_ssid, wifi_pass);
#endif
    wifi_portal_init();   /* reconnects to a saved network if we have one */
}

/* Control server on port 80, Stream server on port 81 */
static void start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    /* 16 control routes + 11 portal routes. Running out silently cost an
     * evening: registration failed part-way and took the portal's 404
     * catch-all with it, so the captive page stopped appearing. */
    config.max_uri_handlers = 40;
    /* The panel polls camera frames and status continuously, so connections
     * churn fast. The default socket count is quickly exhausted and the server
     * then resets connections, which the browser reports as ERR_EMPTY_RESPONSE
     * or ERR_CONNECTION_RESET - and every button on the page starts failing.
     * lru_purge_enable drops the oldest idle socket instead of refusing. */
    config.max_open_sockets = 10;
    /* 8KB, not the 4KB default. The portal's hand-off page builds a ~2.6KB
     * response, and a captive-portal probe from a phone joining the AP runs
     * it on this task - which overflowed the stack and rebooted the robot
     * the moment a phone connected to ORBIE_xxxx. The buffer is static now
     * too, but the margin belongs here: any handler that formats a page is
     * one careless local away from the same crash. */
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    httpd_handle_t ctrl = NULL;
    /* Not ESP_ERROR_CHECK: an unusable socket count aborted here and put the
     * robot in a reboot loop. Falling back to the default is far better than
     * a robot that will not boot. */
    esp_err_t herr = httpd_start(&ctrl, &config);
    if (herr != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed (%s) with max_open_sockets=%d; retrying with defaults",
                 esp_err_to_name(herr), config.max_open_sockets);
        httpd_config_t fallback = HTTPD_DEFAULT_CONFIG();
        fallback.server_port = 80;
        fallback.max_uri_handlers = 40;
        fallback.lru_purge_enable = true;
        herr = httpd_start(&ctrl, &fallback);
        if (herr != ESP_OK) {
            ESP_LOGE(TAG, "control server could not start: %s", esp_err_to_name(herr));
            return;
        }
    }
    httpd_uri_t uris[] = {
            { .uri = "/",       .method = HTTP_GET,  .handler = index_handler },
            { .uri = "/update", .method = HTTP_GET,  .handler = update_page_handler },
            { .uri = "/ota",    .method = HTTP_POST, .handler = ota_handler },
            { .uri = "/motor",  .method = HTTP_GET,  .handler = motor_handler },
            { .uri = "/beep",   .method = HTTP_GET,  .handler = beep_handler },
            { .uri = "/volume", .method = HTTP_GET,  .handler = volume_handler },
            { .uri = "/test",   .method = HTTP_GET,  .handler = test_handler },
            { .uri = "/expr",   .method = HTTP_GET,  .handler = expr_handler },
            { .uri = "/api/whoami", .method = HTTP_GET, .handler = whoami_handler },
            { .uri = "/capture", .method = HTTP_GET,  .handler = capture_handler },
            { .uri = "/say",    .method = HTTP_POST, .handler = say_handler },
            { .uri = "/head",   .method = HTTP_GET,  .handler = head_handler },
            { .uri = "/laser",  .method = HTTP_GET,  .handler = laser_handler },
            { .uri = "/status", .method = HTTP_GET,  .handler = status_handler },
            { .uri = "/api/pose", .method = HTTP_GET, .handler = pose_handler },
            { .uri = "/api/trajectory", .method = HTTP_GET, .handler = trajectory_handler },
            { .uri = "/api/odometry/reset", .method = HTTP_POST, .handler = odometry_reset_handler },
        };
        for (int i = 0; i < (int)(sizeof(uris)/sizeof(uris[0])); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(ctrl, &uris[i]));
    }

    httpd_config_t stream_cfg = HTTPD_DEFAULT_CONFIG();
    stream_cfg.server_port = 81;
    stream_cfg.ctrl_port = 32769;
    stream_cfg.max_uri_handlers = 2;
    stream_cfg.stack_size = 4096;
    httpd_handle_t stream = NULL;
    ESP_ERROR_CHECK(httpd_start(&stream, &stream_cfg));
    httpd_uri_t stream_uri = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler };
    ESP_ERROR_CHECK(httpd_register_uri_handler(stream, &stream_uri));

    wifi_portal_register(ctrl);
    wifi_portal_start_dns();

    /* BLE provisioning runs alongside the captive portal, not instead of it.
     * The app pairs over BLE and never has to leave the owner's Wi-Fi; anyone
     * without the app still gets the portal on the robot's own AP. */
    ble_prov_init();

    ESP_LOGI(TAG, "Servers: port 80 (control), port 81 (MJPEG stream)");
}

static void motor_monitor_task(void *pv) {
    int ticks = 0;
    while (true) {
        motor_safety_check();
        read_temp_sensor();

        /* Sensor telemetry every 2s. Both sensors answer at init - the open
         * question is whether the periodic reads keep working, and a value
         * that never changes looks identical to a dead sensor from the UI. */
        if (++ticks >= 10) {
            ticks = 0;
            /* Integer tenths rather than %f: float formatting is what
             * overflowed this task's stack in the first place. */
            ESP_LOGI(TAG, "sensors: distance=%umm  ambient=%d.%dC  object=%d.%dC  temp_ok=%d",
                     (unsigned)g_distance_mm,
                     (int)temp_ambient, ((int)(temp_ambient * 10)) % 10,
                     (int)temp_object,  ((int)(temp_object  * 10)) % 10,
                     (int)temp_ok);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void vl53_task(void *pv)
{
    VL53L0X *vl = new VL53L0X(I2C_NUM_0);
    /* Use the shared I2C bus instead of creating a new one */
    vl->setBusHandle(i2c_bus);
    vl->addDevice(400000);
    /* Keep trying instead of giving up. The five original attempts all fall
     * inside the window where the boot animation is still driving the LED
     * matrix on the same I2C bus, so a busy bus used to look like a dead
     * sensor - and the giving-up path then crashed the whole robot.
     *
     * `delete vl` was that crash: the VL53L0X destructor touches a device
     * handle that a failed init never set up, which panicked, rebooted, and
     * looped roughly every 25 seconds on both units. The object is
     * deliberately not deleted here; this task owns it for the life of the
     * robot, so leaking it on a path that never returns costs nothing. */
    bool inited = false;
    int round = 0;
    while (!inited) {
        for (int retry = 0; retry < 5 && !inited; retry++) {
            vTaskDelay(pdMS_TO_TICKS(100 * (retry + 1)));
            if (vl->init()) {
                inited = true;
            } else {
                ESP_LOGW(TAG, "VL53L0X init attempt %d/5 failed", retry + 1);
            }
        }
        if (!inited) {
            round++;
            /* Back off, and stop shouting. On a robot whose sensor has failed
             * this never succeeds, and each round is a burst of I2C errors
             * from the driver - retrying every 5s forever filled the console
             * and kept the shared bus busy for a device that is not coming
             * back. Slow to a minute, and say so once. */
            int wait_ms = round < 3 ? 5000 : 60000;
            if (round == 3) {
                ESP_LOGE(TAG, "VL53L0X still absent after %d rounds - retrying quietly every 60s", round);
            } else if (round < 3) {
                ESP_LOGE(TAG, "VL53L0X init failed (round %d) - retrying in 5s", round);
            }
            vTaskDelay(pdMS_TO_TICKS(wait_ms));
        }
    }
    ESP_LOGI(TAG, "VL53L0X ready");

    /* Back off when the sensor stops answering. A unit whose VL53L0X has died
     * still returns false ten times a second, and the driver logs an I2C
     * error for each one - thousands of lines that bury everything else in
     * the console and keep the shared bus busy for no benefit. Slow down
     * instead, and keep trying quietly in case it comes back. */
    int consecutive_failures = 0;
    while (true) {
        uint16_t distance = 0;
        if (vl->read(&distance)) {
            if (consecutive_failures >= 20) {
                ESP_LOGI(TAG, "VL53L0X responding again");
            }
            consecutive_failures = 0;
            g_distance_mm = distance;
            if (g_laser_auto) {
                g_laser_enabled = (distance <= 200);
                gpio_set_level(LASER_GPIO, g_laser_enabled ? 0 : 1); /* active-low */
            }
        } else {
            if (++consecutive_failures == 20) {
                ESP_LOGW(TAG, "VL53L0X not responding - polling every 2s from here");
                g_distance_mm = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(consecutive_failures >= 20 ? 2000 : 100));
    }
}

extern "C" void app_main(void)
{
    /* Let battery stabilize */
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* Log reset reason */
        esp_reset_reason_t reason = esp_reset_reason();
        ESP_LOGI(TAG, "Reset reason: %d", reason);

        /* Disable USB Serial/JTAG to prevent spurious resets on battery
         * (floating D+/D- lines cause USB_UART_CHIP_RESET every ~5s).
         * Skipped in dev builds (-DORBIE_KEEP_USB_CONSOLE=1) so the console
         * stays up for idf.py monitor; keep it ON for untethered demos. */
#ifndef ORBIE_KEEP_USB_CONSOLE
                USB_SERIAL_JTAG.conf0.usb_pad_enable = 0;
                USB_SERIAL_JTAG.conf0.phy_sel = 0;
#else
                ESP_LOGW(TAG, "USB Serial/JTAG kept alive (dev build) - expect "
                              "spurious resets if running on battery");
#endif

    /* Immediately drive all output pins LOW to prevent floating inputs
       from causing motor driver current draw during boot */
    gpio_set_level((gpio_num_t)1, 0);
    gpio_set_level((gpio_num_t)2, 0);
    gpio_set_level((gpio_num_t)3, 0);
    gpio_set_level((gpio_num_t)4, 0);
    gpio_set_level((gpio_num_t)43, 0);
    gpio_set_level((gpio_num_t)44, 0);
    gpio_set_level((gpio_num_t)7, 0);
    gpio_set_level((gpio_num_t)8, 0);
    gpio_set_level((gpio_num_t)9, 0);
    for (int i = 0; i < 10; i++) { gpio_set_direction((gpio_num_t)i, GPIO_MODE_OUTPUT); gpio_set_level((gpio_num_t)i, 0); }

    vTaskDelay(pdMS_TO_TICKS(500));

    esp_err_t ota_valid_err = esp_ota_mark_app_valid_cancel_rollback();
    if (ota_valid_err == ESP_OK) { ESP_LOGI(TAG, "OTA app marked valid"); }
    else if (ota_valid_err != ESP_ERR_NOT_FOUND) { ESP_LOGW(TAG, "OTA validation skipped: 0x%x", ota_valid_err); }

    init_motors();
    start_camera();
    init_laser();
    init_servo();

    /* Initialize shared I2C bus and LED face display */
    init_i2c_bus();
    init_temp_sensor();
    init_display(i2c_bus);


    /* Firmware version on the face, before anything else animates. The only
     * way to confirm an OTA actually landed without plugging in a cable. */
    {
        const esp_app_desc_t *desc = esp_app_get_description();
        char shown[8] = "";
        /* Project version looks like "1.0.4" or a git hash; the face fits four
         * characters, so take the leading digits and dot. */
        /* Digits only, dots dropped: "1.0.0" -> "100", "1.0.1" -> "101".
         * Four characters is not enough for "1.0.1" with separators, and
         * truncating it gives "1.0" - identical to 1.0.0, which would make the
         * version useless for confirming an OTA actually landed. */
        int n = 0;
        for (const char *p = desc->version; *p && n < 4; p++) {
            if (*p >= '0' && *p <= '9') shown[n++] = *p;
            else if (*p != '.' && n > 0) break;   /* stop at a git hash suffix */
        }
        shown[n] = '\0';
        if (n == 0) { strcpy(shown, "----"); }
        ESP_LOGI(TAG, "Firmware version: %s (showing \"%s\")", desc->version, shown);
        display_show_text4(shown, 0, 40, 80);
        vTaskDelay(pdMS_TO_TICKS(1800));
    }

    /* Initialize speaker */
    init_speaker();
    volume_load();   /* before the greeting, which is the loudest thing we play */
    /* NVS must be up before anything reads it. It used to be initialised
     * inside start_wifi_ap(), which runs ~15s later, so both of these loads
     * failed with ESP_ERR_NVS_NOT_INITIALIZED and silently fell back to
     * defaults - the device key looked like it saved, then vanished on every
     * reboot and the server answered 401. Calling it twice is harmless. */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    api_base_load();
    device_key_load();
    ESP_LOGI(TAG, "Volume: %d%%", volume_pct);

    /* Boot animation: LED face + voice greeting playing in parallel */
    xTaskCreate([](void*) { play_boot_sound(); vTaskDelete(NULL); }, "boot_snd", 8192, NULL, 3, NULL);

    display_play_boot_animation();

    /* Demo reel: heart -> star -> loader -> rainbow (~6.8s), runs while the
     * 7.9s greeting is still playing so the two finish together. */
    display_play_demo_sequence();

    start_wifi_ap();
    start_webserver();
    display_look_direction(EYE_CENTER);

    /* 1536 was too tight to log from: %f formatting alone costs over a
     * kilobyte of stack, which overflowed this task and panicked the robot. */
    /* Push-to-talk needs internet, which only exists after the portal has
     * joined a real network - so this is started by the Wi-Fi state, not here.
     * See ptt_watch_task. */
    xTaskCreate([](void *) {
        bool started = false;
        bool announced = false;
        while (true) {
            if (!started && wifi_portal_state() == ORBIE_WIFI_CONNECTED) {
                voice_link_start(api_base, wifi_ssid, device_key,
                                 ptt_play_pcm, ptt_grab_frame, ptt_release_frame);
                started = true;
                ESP_LOGI(TAG, "push-to-talk link started against %s", api_base);
            }

            /* Tell the owner, out loud, the first time we come online.
             *
             * Setting up Wi-Fi from a phone is a leap of faith: you type a
             * password into an app and nothing visible happens. The robot has
             * a speaker and, at this exact moment, its first internet
             * connection - so it can say so in its own voice. The face turns
             * happy at the same time, which is the part that still works if
             * the speaker is dead or synthesis fails. */
            if (started && !announced) {
                announced = true;
                display_play_named("heart");
                /* In its OWN task, with a real stack. voice_link_say() opens a
                 * TLS connection, and an mbedTLS handshake needs 6-8KB -
                 * several times what this watcher was given. Calling it from
                 * here overflowed the stack and rebooted the robot every time
                 * it came online. It also blocks for seconds, which this loop
                 * should not. */
                ESP_LOGI(TAG, "announcing (free internal heap: %u)",
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
                BaseType_t created = xTaskCreate([](void *) {
                    /* Retry. The first attempt lands about two seconds after
                     * the robot gets its IP, and DNS is usually not up yet -
                     * it failed with ESP_ERR_HTTP_CONNECT in 141ms, far too
                     * fast to have tried the network at all. A few seconds of
                     * patience is the whole fix. */
                    bool said = false;
                    for (int attempt = 1; attempt <= 4 && !said; attempt++) {
                        vTaskDelay(pdMS_TO_TICKS(attempt == 1 ? 3000 : 5000));
                        said = voice_link_say("I'm online. Nice to meet you.");
                        if (!said) {
                            ESP_LOGW(TAG, "announce attempt %d/4 failed", attempt);
                        }
                    }
                    if (!said) {
                        ESP_LOGW(TAG, "could not announce - playing a chime instead");
                        play_beep();
                    }
                    vTaskDelete(NULL);
                }, "announce", 12288, NULL, 3, NULL);
                /* A task stack this size competes with BLE, Wi-Fi and the
                 * camera for internal RAM, and xTaskCreate just returns
                 * pdFAIL when it cannot get it - silently, if nobody looks.
                 * That is exactly what happened: the robot came online and
                 * said nothing, with no error anywhere. */
                if (created != pdPASS) {
                    ESP_LOGE(TAG, "announce task would not start - chime instead");
                    play_beep();
                }
            }
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }, "ptt_watch", 4096, NULL, 2, NULL);

    xTaskCreate(motor_monitor_task, "motor_mon", 4096, NULL, 1, NULL);
        xTaskCreate(vl53_task, "vl53", 8192, NULL, 5, NULL);
        xTaskCreate(odometry_task, "odom", 3072, NULL, 3, NULL);
    }