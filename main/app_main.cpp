#include <string.h>
#include <math.h>
#include "esp_camera.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "VL53L0X.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_psram.h"
#include "cJSON.h"
#include "led_display.h"
#include "boot_sound.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
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

#define WIFI_SSID "PAWME-Robot"
#define WIFI_PASS "pawme1234"

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
    httpd_resp_sendstr(req,
"<!doctype html><html><head>"
"<meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no'>"
"<title>PAWME Robot</title>"
"<style>"
"*{margin:0;padding:0;box-sizing:border-box}"
"body{background:#111;color:#eee;font-family:Arial;text-align:center;touch-action:none;overflow:hidden;height:100dvh}"
"#stream{width:100%;max-width:720px;display:block;margin:0 auto}"
"#info{font-size:14px;margin:8px;color:#aaa}"
"#ctrl{position:fixed;bottom:20px;left:0;right:0;display:flex;justify-content:center;gap:16px;padding:10px;background:rgba(0,0,0,0.7);backdrop-filter:blur(8px)}"
".joy{width:80px;height:80px;border-radius:50%;background:rgba(255,255,255,0.06);border:2px solid rgba(255,255,255,0.15);position:relative;touch-action:none}"
".joy .knob{width:32px;height:32px;border-radius:50%;background:#e94560;position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);pointer-events:none;box-shadow:0 0 12px rgba(233,69,96,0.3)}"
".joy .lbl{position:absolute;bottom:-16px;left:0;width:100%;text-align:center;font-size:8px;color:rgba(255,255,255,0.3);text-transform:uppercase;letter-spacing:1px}"
".ab{width:46px;height:46px;border-radius:50%;background:rgba(255,255,255,0.08);border:1px solid rgba(255,255,255,0.12);display:flex;align-items:center;justify-content:center;font-size:10px;cursor:pointer;color:#fff;user-select:none;text-transform:uppercase;font-weight:600;letter-spacing:1px;transition:all .15s}"
".ab:active{transform:scale(0.88)}"
".ab.on{border-color:#e53935;background:rgba(255,20,20,0.15);box-shadow:0 0 12px rgba(255,0,0,0.15)}"
".row{display:flex;gap:10px;align-items:center}"
"</style></head><body>"
"<img id='stream' src='http://192.168.4.1:81/stream'>"
"<div id='info'>Connecting...</div>"
"<div id='ctrl'>"
"<div class='joy' id='drivejoy'><div class='knob' id='driveknob'></div><div class='lbl'>DRIVE</div></div>"
"<div class='row'>"
"<div class='ab' id='btnstop' style='color:#e53935'>STOP</div>"
"<div class='ab' id='btnbeep'>BEEP</div>"
"<div class='ab' id='btnlaser'>LASER</div>"
"</div>"
"<div class='joy' id='turnjoy'><div class='knob' id='turnknob'></div><div class='lbl'>TURN</div></div>"
"</div>"
"<script>"
"(function(){"
"var info=document.getElementById('info');"
"var dk=document.getElementById('driveknob');"
"var tk=document.getElementById('turnknob');"

"function setupV(el,knob,cb){"
"var sy=0;"
"function mv(e){"
"var y=e.touches?e.touches[0].clientY:e.clientY;"
"var dy=y-sy;var v=-dy/30;if(v>1)v=1;if(v<-1)v=-1;"
"knob.style.top=(50+dy)+'px';cb(v);"
"}"
"function up(){knob.style.top='50%';knob.style.left='50%';cb(0);}"
"el.addEventListener('touchstart',function(e){sy=e.touches[0].clientY;mv(e);},{passive:true});"
"el.addEventListener('touchmove',mv,{passive:true});"
"el.addEventListener('touchend',up,{passive:true});"
"el.addEventListener('mousedown',function(e){sy=e.clientY;mv(e);});"
"document.addEventListener('mousemove',function(e){if(sy)mv(e);});"
"document.addEventListener('mouseup',function(){if(sy){sy=0;up();}});"
"}"

"function setupH(el,knob,cb){"
"var sx=0;"
"function mv(e){"
"var x=e.touches?e.touches[0].clientX:e.clientX;"
"var dx=x-sx;var v=dx/30;if(v>1)v=1;if(v<-1)v=-1;"
"knob.style.left=(50+dx)+'%';knob.style.top='50%';cb(v);"
"}"
"function up(){knob.style.top='50%';knob.style.left='50%';cb(0);}"
"el.addEventListener('touchstart',function(e){sx=e.touches[0].clientX;mv(e);},{passive:true});"
"el.addEventListener('touchmove',mv,{passive:true});"
"el.addEventListener('touchend',up,{passive:true});"
"el.addEventListener('mousedown',function(e){sx=e.clientX;mv(e);});"
"document.addEventListener('mousemove',function(e){if(sx)mv(e);});"
"document.addEventListener('mouseup',function(){if(sx){sx=0;up();}});"
"}"

"function go(d,t){fetch('/motor?drive='+Math.round(d*255)+'&turn='+Math.round(t*255)).catch(function(){});}"

"var dv=0,tv=0;"
"setupV(document.getElementById('drivejoy'),dk,function(v){dv=v;go(dv,tv);});"
"setupH(document.getElementById('turnjoy'),tk,function(v){tv=v;go(dv,tv);});"

"document.getElementById('btnstop').onclick=function(){go(0,0);dv=0;tv=0;dk.style.top='50%';dk.style.left='50%';tk.style.top='50%';tk.style.left='50%';};"
"document.getElementById('btnbeep').onclick=function(){fetch('/beep').catch(function(){});};"
"document.getElementById('btnlaser').onclick=function(){"
"var b=this;"
"fetch('/laser').then(function(r){return r.text();}).then(function(t){"
"if(t==='LASER_ON'){b.className='ab on';}else{b.className='ab';}"
"}).catch(function(){});"
"};"

"setInterval(function(){"
"fetch('/status').then(function(r){return r.json();}).then(function(d){"
"info.innerHTML='Dist: '+d.distance+'mm | Amb: '+d.temp_ambient.toFixed(1)+'C Obj: '+d.temp_object.toFixed(1)+'C | Laser: '+(d.laser?'ON':'OFF')+' | D:'+d.drive+' T:'+d.turn;"
"var lb=document.getElementById('btnlaser');"
"if(d.laser){lb.className='ab on';}else{lb.className='ab';}"
"}).catch(function(){});"
"},500);"
"})();"
"</script></body></html>"
    );
    return ESP_OK;
}

static esp_err_t update_page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
"<!doctype html><html><head>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>OTA Update</title>"
"<style>body{background:#111;color:#eee;font-family:Arial;padding:24px}button,input{font-size:16px;margin-top:12px}</style>"
"</head><body><h1>OTA Update</h1>"
"<input id='file' type='file' accept='.bin'>"
"<br><button onclick='u()'>Upload firmware</button>"
"<pre id='st'></pre>"
"<script>async function u(){var f=document.getElementById('file').files[0];if(!f){st.textContent='Choose a .bin file first';return;}st.textContent='Uploading...';var r=await fetch('/ota',{method:'POST',body:f});st.textContent=await r.text();}</script>"
"</body></html>"
    );
    return ESP_OK;
}

static esp_err_t ota_handler(httpd_req_t *req)
{
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
    while (remaining > 0) {
        int received = httpd_req_recv(req, buffer, sizeof(buffer));
        if (received <= 0) {
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload failed");
            return ESP_FAIL;
        }
        esp_ota_write(ota_handle, buffer, received);
        remaining -= received;
    }
    esp_ota_end(ota_handle);
    esp_ota_set_boot_partition(update_partition);
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
        char boundary[] = "--frame\r\nContent-Type: image/jpeg\r\n\r\n";
        if (httpd_resp_send_chunk(req, boundary, strlen(boundary)) != ESP_OK) { break; }
        if (httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len) != ESP_OK) { esp_camera_fb_return(fb); break; }
        if (httpd_resp_send_chunk(req, "\r\n", 2) != ESP_OK) { esp_camera_fb_return(fb); break; }
        esp_camera_fb_return(fb);
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
    if (i2s_channel_init_std_mode(speaker_handle, &std_cfg) != ESP_OK ||
        i2s_channel_enable(speaker_handle) != ESP_OK) {
        ESP_LOGW(TAG, "Speaker I2S init failed");
        i2s_del_channel(speaker_handle);
        speaker_handle = NULL;
        return;
    }
    speaker_ok = true;
    ESP_LOGI(TAG, "Speaker initialized (GPIO44/7/8, %dHz)", BOOT_PCM_SAMPLE_RATE);
}

static void play_boot_sound(void)
{
    if (!speaker_ok || !speaker_handle) return;
    size_t written = 0;
    i2s_channel_write(speaker_handle, boot_pcm_data, BOOT_PCM_NUM_BYTES, &written, portMAX_DELAY);
    ESP_LOGI(TAG, "Boot sound played (%u bytes)", (unsigned)written);
}

static void init_i2c_bus(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = GPIO_NUM_5,
        .scl_io_num = GPIO_NUM_6,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = 1 },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus));
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
    /* Center position: 2425us out of 20000us period * 16384 = 1987 */
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_5, 1987);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_5);
    ESP_LOGI(TAG, "Servo centered on GPIO43");
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

static esp_err_t beep_handler(httpd_req_t *req)
{
    httpd_resp_sendstr(req, "BEEP");
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
    ESP_LOGI(TAG, "Camera ready");
}

static void start_wifi_ap(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_config_t wifi_config = {};
    strcpy((char *)wifi_config.ap.ssid, WIFI_SSID);
    strcpy((char *)wifi_config.ap.password, WIFI_PASS);
    wifi_config.ap.ssid_len = strlen(WIFI_SSID);
    wifi_config.ap.channel = 1;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Wi-Fi AP: %s / %s", WIFI_SSID, WIFI_PASS);
}

/* Control server on port 80, Stream server on port 81 */
static void start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 12;
    httpd_handle_t ctrl = NULL;
    ESP_ERROR_CHECK(httpd_start(&ctrl, &config));
    httpd_uri_t uris[] = {
            { .uri = "/",       .method = HTTP_GET,  .handler = index_handler },
            { .uri = "/update", .method = HTTP_GET,  .handler = update_page_handler },
            { .uri = "/ota",    .method = HTTP_POST, .handler = ota_handler },
            { .uri = "/motor",  .method = HTTP_GET,  .handler = motor_handler },
            { .uri = "/beep",   .method = HTTP_GET,  .handler = beep_handler },
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

    ESP_LOGI(TAG, "Servers: port 80 (control), port 81 (MJPEG stream)");
}

static void motor_monitor_task(void *pv) {
    while (true) {
        motor_safety_check();
        read_temp_sensor();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void vl53_task(void *pv)
{
    VL53L0X *vl = new VL53L0X(I2C_NUM_0);
    /* Use the shared I2C bus instead of creating a new one */
    vl->setBusHandle(i2c_bus);
    vl->addDevice(400000);
    bool inited = false;
    for (int retry = 0; retry < 5; retry++) {
        vTaskDelay(pdMS_TO_TICKS(100 * (retry + 1)));
        if (vl->init()) { inited = true; break; }
        ESP_LOGW(TAG, "VL53L0X init attempt %d/5 failed", retry + 1);
    }
    if (!inited) { ESP_LOGE(TAG, "VL53L0X init failed"); delete vl; vTaskDelete(NULL); return; }
    while (true) {
        uint16_t distance = 0;
        if (vl->read(&distance)) {
            g_distance_mm = distance;
            if (g_laser_auto) {
                g_laser_enabled = (distance <= 200);
                gpio_set_level(LASER_GPIO, g_laser_enabled ? 0 : 1); /* active-low */
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
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
                   (floating D+/D- lines cause USB_UART_CHIP_RESET every ~5s) */
                USB_SERIAL_JTAG.conf0.usb_pad_enable = 0;
                USB_SERIAL_JTAG.conf0.phy_sel = 0;

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

    /* Initialize speaker */
    init_speaker();

    /* Boot animation: LED face + voice greeting playing in parallel */
    xTaskCreate([](void*) { play_boot_sound(); vTaskDelete(NULL); }, "boot_snd", 8192, NULL, 3, NULL);
    display_play_boot_animation();

    start_wifi_ap();
    start_webserver();
    xTaskCreate(motor_monitor_task, "motor_mon", 1536, NULL, 1, NULL);
        xTaskCreate(vl53_task, "vl53", 8192, NULL, 5, NULL);
        xTaskCreate(odometry_task, "odom", 3072, NULL, 3, NULL);
    }