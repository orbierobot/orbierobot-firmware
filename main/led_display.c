#include "led_display.h"
#include "IS31FL3733.h"
#include "boot_expressions.h"
#include "joystick_expressions.h"
#include "expr_look_center_to_bliss.h"
#include "demo_expressions.h"
#include "i2c_lock.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "led_display";

bool display_ok = false;

/* ========== Hardware Drivers ========== */

static is31fl3733_t ic1;  /* Left eye (x=0..7), I2C address 0x50 */
static is31fl3733_t ic2;  /* Right eye (x=8..15), I2C address 0x5F */

/* ========== Framebuffers ========== */

/* Each buffer is 192 bytes: 16 CS lines * 12 SW lines, one byte per LED for PWM.
   We use SW0..SW2 for R,G,B of row0, SW3..SW5 for row1, SW6..SW8 for row2, SW9..SW11 for row3.
   Total: 4 rows * 3 colors * 16 CS = 192 bytes per IC. */

static uint8_t _buf1[192];
static uint8_t _buf2[192];

/* ========== Pixel LUT ========== */

/* Maps pixel index (localX * 8 + y) to {CS, SW_base} */
static const uint8_t PIXEL_LUT[64][2] = {
    {0,0},{1,0},{2,0},{3,0},{4,0},{5,0},{6,0},{7,0},
    {8,0},{9,0},{10,0},{11,0},{12,0},{13,0},{14,0},{15,0},
    {0,3},{1,3},{2,3},{3,3},{4,3},{5,3},{6,3},{7,3},
    {8,3},{9,3},{10,3},{11,3},{12,3},{13,3},{14,3},{15,3},
    {0,6},{1,6},{2,6},{3,6},{4,6},{5,6},{6,6},{7,6},
    {8,6},{9,6},{10,6},{11,6},{12,6},{13,6},{14,6},{15,6},
    {0,9},{1,9},{2,9},{3,9},{4,9},{5,9},{6,9},{7,9},
    {8,9},{9,9},{10,9},{11,9},{12,9},{13,9},{14,9},{15,9},
};

/* ========== CS Mirroring ========== */

static uint8_t mirror_cs(uint8_t cs) {
    return (cs >= 8) ? (15 - cs) + 8 : 7 - cs;
}

/* ========== Public API ========== */

void init_display(i2c_master_bus_handle_t bus)
{
    i2c_lock_init();
    ESP_LOGI(TAG, "Initializing LED face display...");

    /* Init IC1 (left half, x=0..7): ADDR1=GND, ADDR2=GND => 0x50 */
    is31fl3733_init(&ic1, bus, IS31FL3733_ADDR_GND, IS31FL3733_ADDR_GND);

    /* Small delay between IC inits */
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Init IC2 (right half, x=8..15): ADDR1=VCC, ADDR2=VCC => 0x5F */
    is31fl3733_init(&ic2, bus, IS31FL3733_ADDR_VCC, IS31FL3733_ADDR_VCC);

    vTaskDelay(pdMS_TO_TICKS(10));

    /* Set pull-up/down resistors (32k as per PAWME design) */
    is31fl3733_set_swpur(&ic1, IS31FL3733_RES_32K);
    is31fl3733_set_cspdr(&ic1, IS31FL3733_RES_32K);
    is31fl3733_set_swpur(&ic2, IS31FL3733_RES_32K);
    is31fl3733_set_cspdr(&ic2, IS31FL3733_RES_32K);

    /* Enable all LED outputs */
    is31fl3733_set_led_matrix_state(&ic1, IS31FL3733_LED_ON);
    is31fl3733_set_led_matrix_state(&ic2, IS31FL3733_LED_ON);

    vTaskDelay(pdMS_TO_TICKS(5));

    /* Set brightness */
    is31fl3733_set_gcc(&ic1, 128);
    is31fl3733_set_gcc(&ic2, 128);

    /* Clear framebuffers */
    memset(_buf1, 0, sizeof(_buf1));
    memset(_buf2, 0, sizeof(_buf2));

    /* Push black to hardware */
    display_show();

    /* Ask both chips whether they are actually there. Without this display_ok
     * went true even when every single write NACKed, and the animation loop
     * then retried forever - tens of thousands of error lines, a face that
     * never lit, and a robot that looked hung. */
    bool left  = i2c_master_probe(bus, 0x50, 100) == ESP_OK;
    bool right = i2c_master_probe(bus, 0x5f, 100) == ESP_OK;
    display_ok = left && right;

    if (display_ok) {
        ESP_LOGI(TAG, "LED face display ready");
    } else {
        ESP_LOGE(TAG, "LED face display NOT responding (0x50:%s 0x5f:%s) - face disabled",
                 left ? "ok" : "no", right ? "ok" : "no");
    }
}

void display_set_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (x < 0 || x >= 16 || y < 0 || y >= 8) return;

    uint8_t localX;
    uint8_t *buf;

    if (x < 8) {
        localX = (uint8_t)x;
        buf = _buf1;
    } else {
        localX = (uint8_t)(x - 8);
        buf = _buf2;
    }

    uint8_t point = localX * 8 + (uint8_t)y;
    uint8_t cs = PIXEL_LUT[point][0];
    uint8_t sw_base = PIXEL_LUT[point][1];

    /* Apply CS mirroring */
    cs = mirror_cs(cs);

    /* Write R, G, B into framebuffer */
    buf[(sw_base + 0) * 16 + cs] = r;  /* Red */
    buf[(sw_base + 1) * 16 + cs] = g;  /* Green */
    buf[(sw_base + 2) * 16 + cs] = b;  /* Blue */
}

void display_clear(void)
{
    memset(_buf1, 0, sizeof(_buf1));
    memset(_buf2, 0, sizeof(_buf2));
}

void display_show(void)
{
    if (!display_ok) return;
    /* Both ICs in one critical section: a sensor read landing between them
     * leaves the two halves of the face showing different frames. */
    if (!i2c_lock_take(200)) return;   /* drop a frame rather than stall */
    is31fl3733_set_pwm(&ic1, _buf1);
    is31fl3733_set_pwm(&ic2, _buf2);
    i2c_lock_give();
}

void display_draw_rgb_frame(const uint8_t *data)
{
    display_clear();

    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 16; x++) {
            uint16_t idx = (uint16_t)(y * 16 + x) * 3;
            uint8_t eye_offset = (x < 8) ? 0 : 8;
            uint8_t lx = (uint8_t)(x - eye_offset);

            /* Rotate: transpose x/y within each eye, mirror y */
            int new_x = (int)(eye_offset + y);
            int new_y = 7 - (int)lx;

            display_set_pixel(new_x, new_y, data[idx], data[idx + 1], data[idx + 2]);
        }
    }

    display_show();
}

/* ========== Expression/Animation Helpers ========== */

void display_show_frame(const ExprFrame *frame)
{
    display_draw_rgb_frame(frame->data);
}

void display_play_boot_animation(void)
{
    ESP_LOGI(TAG, "Playing boot animation (%u frames wakeup)...", (unsigned)EXPR_WAKEUP_FRAME_COUNT);
    for (unsigned i = 0; i < EXPR_WAKEUP_FRAME_COUNT; i++) {
        display_show_frame(&expr_wakeup_anim[i]);
        vTaskDelay(pdMS_TO_TICKS(expr_wakeup_anim[i].duration_ms));
    }

    ESP_LOGI(TAG, "Playing blink (%u frames)...", (unsigned)EXPR_BLINK_FRAME_COUNT);
    for (unsigned i = 0; i < EXPR_BLINK_FRAME_COUNT; i++) {
        display_show_frame(&expr_blink_anim[i]);
        vTaskDelay(pdMS_TO_TICKS(expr_blink_anim[i].duration_ms));
    }

    ESP_LOGI(TAG, "Playing look_left_right (%u frames)...", (unsigned)EXPR_LOOK_LR_FRAME_COUNT);
    for (unsigned i = 0; i < EXPR_LOOK_LR_FRAME_COUNT; i++) {
        display_show_frame(&expr_look_lr_anim[i]);
        vTaskDelay(pdMS_TO_TICKS(expr_look_lr_anim[i].duration_ms));
    }

    /* Pulsing red hearts, then settle on neutral centre */
    ESP_LOGI(TAG, "Showing heart...");
    display_play_heart(2);

    ESP_LOGI(TAG, "Boot animation complete — neutral face");
    display_draw_rgb_frame(eyeDirectionFrames[EYE_CENTER]);
}

/* 3x5 glyphs, one byte per row, low 3 bits used. Small enough to read on a
 * 16x8 face and still fit four characters. */
typedef struct { char ch; uint8_t rows[5]; } glyph_t;

static const glyph_t FONT[] = {
    {'0', {0b111,0b101,0b101,0b101,0b111}},
    {'1', {0b010,0b110,0b010,0b010,0b111}},
    {'2', {0b111,0b001,0b111,0b100,0b111}},
    {'3', {0b111,0b001,0b111,0b001,0b111}},
    {'4', {0b101,0b101,0b111,0b001,0b001}},
    {'5', {0b111,0b100,0b111,0b001,0b111}},
    {'6', {0b111,0b100,0b111,0b101,0b111}},
    {'7', {0b111,0b001,0b001,0b001,0b001}},
    {'8', {0b111,0b101,0b111,0b101,0b111}},
    {'9', {0b111,0b101,0b111,0b001,0b111}},
    {'.', {0b000,0b000,0b000,0b000,0b010}},
    {'-', {0b000,0b000,0b111,0b000,0b000}},
    {'v', {0b000,0b101,0b101,0b101,0b010}},
    {' ', {0b000,0b000,0b000,0b000,0b000}},
};

static const uint8_t *glyph_for(char ch)
{
    for (unsigned i = 0; i < sizeof(FONT) / sizeof(FONT[0]); i++) {
        if (FONT[i].ch == ch) return FONT[i].rows;
    }
    return NULL;
}

void display_show_text4(const char *text, uint8_t r, uint8_t g, uint8_t b)
{
    /* Build a logical 16x8 frame and hand it to display_draw_rgb_frame, which
     * applies the eye rotation. Writing through display_set_pixel directly
     * would put the glyphs in physical space and render them sideways. */
    static uint8_t frame[16 * 8 * 3];
    memset(frame, 0, sizeof(frame));

    /* 4 glyphs x 4 columns (3 wide + 1 gap) fills 16px exactly; 5 rows
     * centred vertically in 8. */
    for (int i = 0; i < 4 && text[i]; i++) {
        const uint8_t *rows = glyph_for(text[i]);
        if (!rows) continue;
        for (int gy = 0; gy < 5; gy++) {
            for (int gx = 0; gx < 3; gx++) {
                if (!(rows[gy] & (1 << (2 - gx)))) continue;
                int x = i * 4 + gx;
                int y = gy + 1;
                size_t idx = (size_t)(y * 16 + x) * 3;
                frame[idx + 0] = r;
                frame[idx + 1] = g;
                frame[idx + 2] = b;
            }
        }
    }
    display_draw_rgb_frame(frame);
}

void display_solid(uint8_t r, uint8_t g, uint8_t b)
{
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 16; x++) {
            display_set_pixel(x, y, r, g, b);
        }
    }
    display_show();
}

/* ========== Demo Expressions ========== */

static void play_anim(const ExprFrame *anim, unsigned count, int loops)
{
    for (int l = 0; l < loops; l++) {
        for (unsigned i = 0; i < count; i++) {
            display_show_frame(&anim[i]);
            vTaskDelay(pdMS_TO_TICKS(anim[i].duration_ms));
        }
    }
}

void display_play_heart(int loops)
{
    play_anim(expr_heart_anim, EXPR_HEART_FRAME_COUNT, loops);
}

void display_play_star(int loops)
{
    play_anim(expr_star_anim, EXPR_STAR_FRAME_COUNT, loops);
}

void display_play_loader(int loops)
{
    play_anim(expr_loader_anim, EXPR_LOADER_FRAME_COUNT, loops);
}

void display_play_rainbow(int loops)
{
    play_anim(expr_rainbow_anim, EXPR_RAINBOW_FRAME_COUNT, loops);
}

bool display_play_named(const char *name)
{
    if      (!strcmp(name, "heart"))   display_play_heart(3);
    else if (!strcmp(name, "star"))    display_play_star(3);
    else if (!strcmp(name, "loader"))  display_play_loader(3);
    else if (!strcmp(name, "rainbow")) display_play_rainbow(2);
    else if (!strcmp(name, "blink"))   display_play_blink();
    else if (!strcmp(name, "bliss"))   display_play_center_to_bliss();
    else if (!strcmp(name, "wakeup"))  display_play_boot_animation();
    else if (!strcmp(name, "centre") || !strcmp(name, "center"))
                                       display_look_direction(EYE_CENTER);
    else if (!strcmp(name, "up"))      display_look_direction(EYE_UP);
    else if (!strcmp(name, "down"))    display_look_direction(EYE_DOWN);
    else if (!strcmp(name, "left"))    display_look_direction(EYE_LEFT);
    else if (!strcmp(name, "right"))   display_look_direction(EYE_RIGHT);
    else return false;
    return true;
}

void display_play_demo_sequence(void)
{
    ESP_LOGI(TAG, "Demo reel: heart -> star -> loader -> rainbow");
    display_play_heart(2);
    display_play_star(2);
    display_play_loader(2);
    display_play_rainbow(1);
    display_draw_rgb_frame(eyeDirectionFrames[EYE_CENTER]);
    ESP_LOGI(TAG, "Demo reel complete");
}

void display_look_direction(enum EyeDirection dir)
{
    if (dir >= EYE_CENTER && dir <= EYE_RIGHT) {
        display_draw_rgb_frame(eyeDirectionFrames[dir]);
    }
}

void display_play_blink(void)
{
    for (unsigned i = 0; i < EXPR_BLINK_FRAME_COUNT; i++) {
        display_show_frame(&expr_blink_anim[i]);
        vTaskDelay(pdMS_TO_TICKS(expr_blink_anim[i].duration_ms));
    }
}

void display_play_center_to_bliss(void)
{
    ESP_LOGI(TAG, "Playing center_to_bliss (%u frames)...", (unsigned)EXPR_LOOK_CENTER_TO_BLISS_FRAME_COUNT);
    for (unsigned i = 0; i < EXPR_LOOK_CENTER_TO_BLISS_FRAME_COUNT; i++) {
        display_show_frame(&expr_look_center_to_bliss_anim[i]);
        vTaskDelay(pdMS_TO_TICKS(expr_look_center_to_bliss_anim[i].duration_ms));
    }
}