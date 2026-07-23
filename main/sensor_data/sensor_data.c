#include "sensor_data.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

static sensor_env_t s_env = { .temp = 0, .humi = 0, .timestamp = 0, .valid = false };
static SemaphoreHandle_t s_mutex = NULL;

esp_err_t sensor_data_init(void) {
    if (s_mutex) return ESP_OK;
    s_mutex = xSemaphoreCreateMutex();
    return s_mutex ? ESP_OK : ESP_FAIL;
}

void sensor_data_update(float temp, float humi) {
    if (!s_mutex) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_env.temp      = temp;
        s_env.humi      = humi;
        s_env.timestamp = (uint32_t)(esp_timer_get_time() / 1000);
        s_env.valid     = true;
        xSemaphoreGive(s_mutex);
    }
}

void sensor_data_invalidate(void) {
    if (!s_mutex) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_env.valid = false;
        xSemaphoreGive(s_mutex);
    }
}

sensor_env_t sensor_data_get(void) {
    sensor_env_t copy = { .valid = false };
    if (!s_mutex) return copy;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        copy = s_env;
        xSemaphoreGive(s_mutex);
    }
    return copy;
}
