#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========== Eye Direction Enum ========== */

enum EyeDirection {
    EYE_CENTER = 0,
    EYE_UP,
    EYE_DOWN,
    EYE_LEFT,
    EYE_RIGHT
};

/* ========== Expression Frame Struct ========== */

typedef struct {
    const uint8_t *data;
    uint16_t duration_ms;
} ExprFrame;

/* ========== Display State ========== */

extern bool display_ok;

/* ========== Public API ========== */

/**
 * @brief Initialize the dual-IS31FL3733 LED face display on shared I2C bus.
 * @param bus Shared I2C master bus handle (GPIO5=SCL, GPIO6=SDA, 400kHz)
 */
void init_display(i2c_master_bus_handle_t bus);

/**
 * @brief Set a single pixel in the 16x8 framebuffer.
 * @param x Column (0..15, 0-7=left eye, 8-15=right eye)
 * @param y Row (0..7)
 * @param r Red PWM value (0-255)
 * @param g Green PWM value (0-255)
 * @param b Blue PWM value (0-255)
 */
void display_set_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Clear both framebuffers (set all pixels to black).
 */
void display_clear(void);

/**
 * @brief Push both framebuffers to the IS31FL3733 hardware.
 */
void display_show(void);

/**
 * @brief Draw a full 16x8 RGB frame directly from data.
 *        Data is row-major: index = (y * 16 + x) * 3, values are R,G,B.
 *        Automatically applies eye-rotation (transpose x/y within each eye, mirror y).
 * @param data Pointer to 384 bytes of RGB pixel data (16*8*3)
 */
void display_draw_rgb_frame(const uint8_t *data);

/* ========== Expression/Animation Helpers ========== */

/**
 * @brief Display a single expression frame on the LED face.
 * @param frame Pointer to ExprFrame containing raw 16x8 RGB data.
 */
void display_show_frame(const ExprFrame *frame);

/**
 * @brief Play a boot animation (wakeup -> blink -> look_left_right).
 *        Blocks for the duration of the animation.
 */
void display_play_boot_animation(void);

/**
 * @brief Set eye look direction from the joystick expressions.
 * @param dir EyeDirection enum value (CENTER, UP, DOWN, LEFT, RIGHT)
 */
void display_look_direction(enum EyeDirection dir);

/**
 * @brief Play the blink animation once. Non-blocking if called from a task.
 *        Blocks for ~1.1s.
 */
void display_play_blink(void);

/**
 * @brief Play the center-to-bliss transition animation.
 *        Blocks for the duration (~4.5s).
 */
void display_play_center_to_bliss(void);

#ifdef __cplusplus
}
#endif