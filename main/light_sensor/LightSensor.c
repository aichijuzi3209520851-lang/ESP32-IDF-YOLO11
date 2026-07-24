#include "LightSensor.h"

#include "adc1_shared.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include <stdbool.h>
#include <stdint.h>

#define LIGHT_SENSOR_ADC_CHANNEL  ADC_CHANNEL_1
#define LIGHT_SENSOR_ADC_ATTEN    ADC_ATTEN_DB_12
#define LIGHT_SENSOR_ADC_WIDTH    ADC_BITWIDTH_12
#define LIGHT_SENSOR_SAMPLE_COUNT 32
#define LIGHT_SENSOR_ADC_MAX      4095
#define LIGHT_SENSOR_SUPPLY_MV    3300

static const char *TAG = "LIGHT_SENSOR";
static bool s_initialized;

esp_err_t light_sensor_init(void)
{
    if (s_initialized) return ESP_OK;

    esp_err_t err = adc1_shared_config_channel(
        LIGHT_SENSOR_ADC_CHANNEL,
        LIGHT_SENSOR_ADC_ATTEN,
        LIGHT_SENSOR_ADC_WIDTH);
    if (err != ESP_OK) return err;

    s_initialized = true;
    ESP_LOGI(TAG, "Initialized: AO=GPIO%d, ADC1_CH1, DO unused",
             LIGHT_SENSOR_AO_GPIO);
    return ESP_OK;
}

esp_err_t light_sensor_read(light_sensor_data_t *data)
{
    if (!data) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    int64_t raw_sum = 0;
    for (int i = 0; i < LIGHT_SENSOR_SAMPLE_COUNT; ++i) {
        int raw = 0;
        esp_err_t err = adc1_shared_read(LIGHT_SENSOR_ADC_CHANNEL, &raw);
        if (err != ESP_OK) return err;
        raw_sum += raw;
    }

    data->raw = (int)(raw_sum / LIGHT_SENSOR_SAMPLE_COUNT);
    data->voltage_mv = data->raw * LIGHT_SENSOR_SUPPLY_MV / LIGHT_SENSOR_ADC_MAX;
    data->intensity_percent = (float)data->raw * 100.0f / LIGHT_SENSOR_ADC_MAX;
    return ESP_OK;
}
