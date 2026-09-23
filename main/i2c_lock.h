#pragma once
/* One lock for the shared I2C bus (GPIO5/6).
 *
 * The LED matrix, the MLX90614 and the VL53L0X all sit on it, driven from
 * different tasks: the face from whatever is animating, the temperature from
 * motor_monitor_task, the distance from vl53_task, and any of them from an
 * HTTP handler. ESP-IDF serialises individual transactions, but a failed one
 * can leave the bus in ESP_ERR_INVALID_STATE, and the next caller then fails
 * too - which is how a single NACK turns into a storm and the face stops
 * updating altogether.
 *
 * Holding this across a whole logical operation (a full framebuffer push, a
 * two-register temperature read) keeps those operations from interleaving.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

void i2c_lock_init(void);

/** @return true if taken; false on timeout - skip the work rather than block. */
bool i2c_lock_take(int timeout_ms);
void i2c_lock_give(void);

#ifdef __cplusplus
}
#endif
