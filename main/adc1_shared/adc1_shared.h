#pragma once

#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t adc1_shared_config_channel(adc_channel_t channel,
                                     adc_atten_t atten,
                                     adc_bitwidth_t bitwidth);
esp_err_t adc1_shared_read(adc_channel_t channel, int *raw);

#ifdef __cplusplus
}
#endif
