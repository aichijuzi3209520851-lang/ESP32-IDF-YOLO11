#pragma once

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Four-pin LDR/LM393 module: AO -> GPIO2 (ADC1_CH1), DO is unused. */
#define LIGHT_SENSOR_AO_GPIO GPIO_NUM_2

typedef struct {
    int raw;
    int voltage_mv;
    float intensity_percent;
} light_sensor_data_t;

esp_err_t light_sensor_init(void);
esp_err_t light_sensor_read(light_sensor_data_t *data);

#ifdef __cplusplus
}
#endif
