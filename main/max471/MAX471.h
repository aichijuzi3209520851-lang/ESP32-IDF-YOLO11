#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* MAX471 OUT 接 ESP32-S3 GPIO1，即 ADC1_CH0。 */
#define MAX471_OUT_GPIO 1

esp_err_t max471_init(void);
esp_err_t max471_read_voltage_mv(int *voltage_mv);
esp_err_t max471_read_current_ma(float *current_ma);

/* 兼容项目中原有的函数名。 */
void TEST_ADC_Init(void);
int TEST_ADC_GetVoltage_mv(void);

#ifdef __cplusplus
}
#endif
