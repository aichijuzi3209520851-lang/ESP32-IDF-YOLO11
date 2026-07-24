#include "adc1_shared.h"

static adc_oneshot_unit_handle_t s_adc1_handle;

static esp_err_t adc1_shared_init(void)
{
    if (s_adc1_handle) return ESP_OK;

    const adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    return adc_oneshot_new_unit(&unit_cfg, &s_adc1_handle);
}

esp_err_t adc1_shared_config_channel(adc_channel_t channel,
                                     adc_atten_t atten,
                                     adc_bitwidth_t bitwidth)
{
    esp_err_t err = adc1_shared_init();
    if (err != ESP_OK) return err;

    const adc_oneshot_chan_cfg_t channel_cfg = {
        .atten = atten,
        .bitwidth = bitwidth,
    };
    return adc_oneshot_config_channel(s_adc1_handle, channel, &channel_cfg);
}

esp_err_t adc1_shared_read(adc_channel_t channel, int *raw)
{
    if (!raw) return ESP_ERR_INVALID_ARG;
    if (!s_adc1_handle) return ESP_ERR_INVALID_STATE;
    return adc_oneshot_read(s_adc1_handle, channel, raw);
}
