#pragma once

#include <stdint.h>
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========== Register Definitions ========== */

/* Common registers */
#define IS31FL3733_PSR  0xFD  /* Page Select Register */
#define IS31FL3733_PSWL 0xFE  /* Page Select Write Lock */
#define IS31FL3733_IMR  0xF0  /* Interrupt Mask Register */
#define IS31FL3733_ISR  0xF1  /* Interrupt Status Register */

/* Paged registers (high byte = page, low byte = register offset) */
#define IS31FL3733_LEDONOFF   0x0000  /* Page 0: LED on/off state */
#define IS31FL3733_LEDOPEN    0x0018  /* Page 0: LED open status */
#define IS31FL3733_LEDSHORT   0x0030  /* Page 0: LED short status */
#define IS31FL3733_LEDPWM     0x0100  /* Page 1: PWM duty cycle */
#define IS31FL3733_LEDABM     0x0200  /* Page 2: Auto breath mode */
#define IS31FL3733_CR         0x0300  /* Page 3: Configuration register */
#define IS31FL3733_GCC        0x0301  /* Page 3: Global current control */
#define IS31FL3733_ABM1CR     0x0302  /* Page 3: ABM-1 control */
#define IS31FL3733_ABM2CR     0x0306  /* Page 3: ABM-2 control */
#define IS31FL3733_ABM3CR     0x030A  /* Page 3: ABM-3 control */
#define IS31FL3733_TUR        0x030E  /* Page 3: Time update register */
#define IS31FL3733_SWPUR      0x030F  /* Page 3: SW pull-up resistor */
#define IS31FL3733_CSPDR      0x0310  /* Page 3: CS pull-down resistor */
#define IS31FL3733_RESET      0x0311  /* Page 3: Reset register */

/* PSWL unlock key */
#define IS31FL3733_PSWL_ENABLE  0xC5
#define IS31FL3733_PSWL_DISABLE 0x00

/* Configuration register bits */
#define IS31FL3733_CR_SSD        0x01  /* Software shutdown */
#define IS31FL3733_CR_BEN        0x02  /* Auto breath mode enable */
#define IS31FL3733_CR_OSD        0x04  /* Open/short detection */
#define IS31FL3733_CR_SYNC_MASTER 0x40 /* Clock master */
#define IS31FL3733_CR_SYNC_SLAVE 0x80  /* Clock slave */

/* I2C base address (8-bit) */
#define IS31FL3733_I2C_BASE_ADDR 0xA0

/* LED state */
#define IS31FL3733_LED_OFF 0
#define IS31FL3733_LED_ON  1

/* Resistor values for SWPUR/CSPDR */
#define IS31FL3733_RES_OFF  0
#define IS31FL3733_RES_500  1
#define IS31FL3733_RES_1K   2
#define IS31FL3733_RES_2K   3
#define IS31FL3733_RES_4K   4
#define IS31FL3733_RES_8K   5
#define IS31FL3733_RES_16K  6
#define IS31FL3733_RES_32K  7

/* ADDR pin connection values */
#define IS31FL3733_ADDR_GND 0
#define IS31FL3733_ADDR_SCL 1
#define IS31FL3733_ADDR_SDA 2
#define IS31FL3733_ADDR_VCC 3

/* ========== Driver Struct ========== */

typedef struct {
    uint8_t address;              /* 7-bit I2C address */
    uint8_t leds[24];             /* Internal LED state buffer (192/8 = 24 bytes) */
    i2c_master_bus_handle_t bus;  /* Shared I2C bus handle */
    i2c_master_dev_handle_t dev;  /* Device handle (for this IC) */
} is31fl3733_t;

/* ========== Driver Functions ========== */

/**
 * @brief Calculate 7-bit I2C address from ADDR1 and ADDR2 pin connections.
 * @param addr1 ADDR1 pin connection (IS31FL3733_ADDR_GND/SCL/SDA/VCC)
 * @param addr2 ADDR2 pin connection
 * @return 7-bit I2C address
 */
uint8_t is31fl3733_calc_address(uint8_t addr1, uint8_t addr2);

/**
 * @brief Initialize the IS31FL3733 driver struct and add I2C device.
 * @param ic Pointer to driver instance
 * @param bus Shared I2C bus handle
 * @param addr1 ADDR1 pin connection value
 * @param addr2 ADDR2 pin connection value
 */
void is31fl3733_init(is31fl3733_t *ic, i2c_master_bus_handle_t bus, uint8_t addr1, uint8_t addr2);

/**
 * @brief Set SW pull-up resistor value.
 */
void is31fl3733_set_swpur(is31fl3733_t *ic, uint8_t resistor);

/**
 * @brief Set CS pull-down resistor value.
 */
void is31fl3733_set_cspdr(is31fl3733_t *ic, uint8_t resistor);

/**
 * @brief Set global current control (brightness).
 * @param ic Pointer to driver instance
 * @param gcc Global current control value (0-255)
 */
void is31fl3733_set_gcc(is31fl3733_t *ic, uint8_t gcc);

/**
 * @brief Set all LEDs in the matrix on or off.
 * @param ic Pointer to driver instance
 * @param state IS31FL3733_LED_ON or IS31FL3733_LED_OFF
 */
void is31fl3733_set_led_matrix_state(is31fl3733_t *ic, uint8_t state);

/**
 * @brief Set PWM values for all 192 LEDs from a buffer.
 * @param ic Pointer to driver instance
 * @param values Array of 192 PWM values (0-255 each)
 */
void is31fl3733_set_pwm(is31fl3733_t *ic, const uint8_t *values);

#ifdef __cplusplus
}
#endif