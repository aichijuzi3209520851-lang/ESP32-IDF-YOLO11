#include "MAX471.h"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include <stdbool.h>

#define MAX471_ADC_UNIT             ADC_UNIT_1
#define MAX471_ADC_CHANNEL          ADC_CHANNEL_0
#define MAX471_ADC_ATTEN            ADC_ATTEN_DB_12
#define MAX471_ADC_WIDTH            ADC_BITWIDTH_12
#define MAX471_SAMPLE_COUNT         32
#define MAX471_OUT_MV_PER_AMP       1000.0f /* 常见 MAX471 模块：1 V/A */

static const char *TAG = "MAX471";
static adc_oneshot_unit_handle_t s_adc_handle;
static adc_cali_handle_t s_cali_handle;
static bool s_initialized;
static bool s_calibrated;

esp_err_t max471_init(void) {
    if (s_initialized) return ESP_OK;

    const adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = MAX471_ADC_UNIT,
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    const adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = MAX471_ADC_ATTEN,
        .bitwidth = MAX471_ADC_WIDTH,
    };

    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
    if (err != ESP_OK) return err;

    err = adc_oneshot_config_channel(s_adc_handle, MAX471_ADC_CHANNEL, &chan_cfg);
    if (err != ESP_OK) return err;

    adc_cali_scheme_ver_t scheme;
    if (adc_cali_check_scheme(&scheme) == ESP_OK) {
        err = adc_cali_create_scheme_curve_fitting(
            &(adc_cali_curve_fitting_config_t){
                .unit_id = MAX471_ADC_UNIT,
                .chan = MAX471_ADC_CHANNEL,
                .atten = MAX471_ADC_ATTEN,
                .bitwidth = MAX471_ADC_WIDTH,
            },
            &s_cali_handle);
        if (err == ESP_OK) {
            s_calibrated = true;
        } else {
            ESP_LOGW(TAG, "ADC calibration unavailable: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "ADC calibration scheme not supported; using nominal conversion");
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Initialized: OUT=GPIO%d, ADC1_CH0, calibrated=%s",
             MAX471_OUT_GPIO, s_calibrated ? "yes" : "no");
    return ESP_OK;
}

esp_err_t max471_read_voltage_mv(int *voltage_mv) {
    if (!voltage_mv) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    int64_t raw_sum = 0;
    for (int i = 0; i < MAX471_SAMPLE_COUNT; ++i) {
        int raw = 0;
        esp_err_t err = adc_oneshot_read(s_adc_handle, MAX471_ADC_CHANNEL, &raw);
        if (err != ESP_OK) return err;
        raw_sum += raw;
    }

    int raw_average = (int)(raw_sum / MAX471_SAMPLE_COUNT);
    if (s_calibrated) {
        return adc_cali_raw_to_voltage(s_cali_handle, raw_average, voltage_mv);
    }

    /* 仅作为无校准方案的后备值，正式测量优先使用曲线校准。 */
    *voltage_mv = raw_average * 3300 / 4095;
    return ESP_OK;
}

esp_err_t max471_read_current_ma(float *current_ma) {
    if (!current_ma) return ESP_ERR_INVALID_ARG;

    int voltage_mv = 0;
    esp_err_t err = max471_read_voltage_mv(&voltage_mv);
    if (err != ESP_OK) return err;

    /* I(mA) = Vout(mV) / (1000mV/A) × 1000mA/A。 */
    *current_ma = (float)voltage_mv * 1000.0f / MAX471_OUT_MV_PER_AMP;
    ESP_LOGI(TAG, "MAX471: OUT=%d mV, current=%.1f mA", voltage_mv, *current_ma);
    return ESP_OK;
}

void TEST_ADC_Init(void) {
    ESP_ERROR_CHECK(max471_init());
}

int TEST_ADC_GetVoltage_mv(void) {
    int voltage_mv = 0;
    return max471_read_voltage_mv(&voltage_mv) == ESP_OK ? voltage_mv : 0;
}
