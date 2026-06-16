#include "IS31FL3733.h"
#include "esp_log.h"

static const char *TAG = "IS31FL3733";

/* ========== I2C Helper Functions ========== */

static esp_err_t write_reg(is31fl3733_t *ic, uint8_t reg_addr, uint8_t data)
{
    uint8_t buf[2] = {reg_addr, data};
    return i2c_master_transmit(ic->dev, buf, 2, 100);
}

static esp_err_t write_regs(is31fl3733_t *ic, uint8_t reg_addr, const uint8_t *data, size_t count)
{
    /* Use a fixed 129-byte buffer (1 reg + 128 data max per chunk) */
    uint8_t buf[129];

    size_t remaining = count;
    size_t offset = 0;

    while (remaining > 0) {
        size_t chunk = (remaining > 128) ? 128 : remaining;
        buf[0] = reg_addr + offset;
        for (size_t i = 0; i < chunk; i++) {
            buf[1 + i] = data[offset + i];
        }
        esp_err_t err = i2c_master_transmit(ic->dev, buf, chunk + 1, 100);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2C write failed at offset %u: %s", (unsigned)offset, esp_err_to_name(err));
            return err;
        }
        remaining -= chunk;
        offset += chunk;
    }
    return ESP_OK;
}

static uint8_t read_reg(is31fl3733_t *ic, uint8_t reg_addr)
{
    uint8_t buf[1] = {0};
    esp_err_t err = i2c_master_transmit_receive(ic->dev, &reg_addr, 1, buf, 1, 100);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2C read failed at reg 0x%02x: %s", reg_addr, esp_err_to_name(err));
        return 0;
    }
    return buf[0];
}

/* ========== Page Selection ========== */

static void select_page(is31fl3733_t *ic, uint16_t paged_reg)
{
    /* Unlock page select register */
    write_reg(ic, IS31FL3733_PSWL, IS31FL3733_PSWL_ENABLE);
    /* Select the page (high byte of paged register) */
    write_reg(ic, IS31FL3733_PSR, (uint8_t)(paged_reg >> 8));
}

/* ========== Public Functions ========== */

uint8_t is31fl3733_calc_address(uint8_t addr1, uint8_t addr2)
{
    /* Base is 0xA0 (8-bit), shift right by 1 for 7-bit address */
    return (uint8_t)((IS31FL3733_I2C_BASE_ADDR | ((addr2) << 3) | ((addr1) << 1)) >> 1);
}

void is31fl3733_init(is31fl3733_t *ic, i2c_master_bus_handle_t bus, uint8_t addr1, uint8_t addr2)
{
    ic->bus = bus;
    ic->address = is31fl3733_calc_address(addr1, addr2);

    /* Add device on the shared bus */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ic->address,
        .scl_speed_hz = 400000,
    };

    esp_err_t err = i2c_master_bus_add_device(ic->bus, &dev_cfg, &ic->dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device at 0x%02x: %s", ic->address, esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "IS31FL3733 at 0x%02x initialized", ic->address);

    /* Initialize internal LED state buffer */
    for (int i = 0; i < 24; i++) {
        ic->leds[i] = 0;
    }

    /* Init sequence:
     * 1. Read RESET register to reset device
     * 2. Write CR with CR_SSD (clear software shutdown)
     * 3. Set all LEDs to OFF
     */

    /* Step 1: Reset by reading the RESET register */
    select_page(ic, IS31FL3733_RESET);
    uint8_t reg_low = (uint8_t)(IS31FL3733_RESET & 0xFF);
    read_reg(ic, reg_low);

    /* Small delay after reset */
    esp_rom_delay_us(1000);

    /* Step 2: Clear software shutdown */
    select_page(ic, IS31FL3733_CR);
    reg_low = (uint8_t)(IS31FL3733_CR & 0xFF);
    write_reg(ic, reg_low, IS31FL3733_CR_SSD);

    /* Step 3: Set all LEDs to OFF */
    is31fl3733_set_led_matrix_state(ic, IS31FL3733_LED_OFF);
}

void is31fl3733_set_swpur(is31fl3733_t *ic, uint8_t resistor)
{
    select_page(ic, IS31FL3733_SWPUR);
    uint8_t reg = (uint8_t)(IS31FL3733_SWPUR & 0xFF);
    write_reg(ic, reg, resistor);
}

void is31fl3733_set_cspdr(is31fl3733_t *ic, uint8_t resistor)
{
    select_page(ic, IS31FL3733_CSPDR);
    uint8_t reg = (uint8_t)(IS31FL3733_CSPDR & 0xFF);
    write_reg(ic, reg, resistor);
}

void is31fl3733_set_gcc(is31fl3733_t *ic, uint8_t gcc)
{
    select_page(ic, IS31FL3733_GCC);
    uint8_t reg_low = (uint8_t)(IS31FL3733_GCC & 0xFF);
    write_reg(ic, reg_low, gcc);
}

void is31fl3733_set_led_matrix_state(is31fl3733_t *ic, uint8_t state)
{
    /* Update internal buffer */
    uint8_t fill = (state == IS31FL3733_LED_ON) ? 0xFF : 0x00;
    for (int i = 0; i < 24; i++) {
        ic->leds[i] = fill;
    }

    /* Write to device register (24 bytes = 192 bits for 192 LEDs) */
    select_page(ic, IS31FL3733_LEDONOFF);
    uint8_t reg_low = (uint8_t)(IS31FL3733_LEDONOFF & 0xFF);
    write_regs(ic, reg_low, ic->leds, 24);
}

void is31fl3733_set_pwm(is31fl3733_t *ic, const uint8_t *values)
{
    /* Write 192 PWM bytes to LEDPWM page starting at offset 0 */
    select_page(ic, IS31FL3733_LEDPWM);
    uint8_t reg_low = (uint8_t)(IS31FL3733_LEDPWM & 0xFF);
    write_regs(ic, reg_low, values, 192);
}