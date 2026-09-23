#include "i2c_lock.h"
#include "esp_log.h"

static SemaphoreHandle_t s_lock = NULL;

void i2c_lock_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

bool i2c_lock_take(int timeout_ms)
{
    if (!s_lock) return true;          /* before init: nothing to contend with */
    return xSemaphoreTake(s_lock, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void i2c_lock_give(void)
{
    if (s_lock) xSemaphoreGive(s_lock);
}
