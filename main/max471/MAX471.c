#include <stdio.h>
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_check.h"

#define ADC_UNIT            ADC_UNIT_1
#define ADC_CHAN            ADC_CHANNEL_0
#define ADC_ATTEN           ADC_ATTEN_DB_12
#define ADC_WIDTH           ADC_BITWIDTH_12

/* 1. 句柄放全局，任务里直接读 */
static adc_oneshot_unit_handle_t  adc_hdl;
static adc_cali_handle_t          cal_hdl;

/* 2. 配置常量区，只读不占栈 */
static const adc_oneshot_unit_init_cfg_t init_cfg = {
    .unit_id  = ADC_UNIT,
    .clk_src  = ADC_RTC_CLK_SRC_DEFAULT,
    .ulp_mode = ADC_ULP_MODE_DISABLE,
};

static const adc_oneshot_chan_cfg_t ch_cfg = {
    .atten    = ADC_ATTEN,
    .bitwidth = ADC_WIDTH,
};

static const adc_cali_curve_fitting_config_t cal_cfg = {
    .unit_id  = ADC_UNIT,
    .chan     = ADC_CHAN,
    .atten    = ADC_ATTEN,
    .bitwidth = ADC_WIDTH,
};

/* ---------------------------------------------------- */

void TEST_ADC_Init(void)
{
    /* 一次性初始化，失败直接断言 */
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &adc_hdl));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_hdl, ADC_CHAN, &ch_cfg));

    /* 如果芯片支持曲线校准才创建 */
    adc_cali_scheme_ver_t scheme;
    if (adc_cali_check_scheme(&scheme) == ESP_OK) {
        ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cal_cfg, &cal_hdl));
    }
}

/* ---------------------------------------------------- */
/* 3. 读 ADC 函数：栈只消耗 2 个 int = 8 Byte           */
int TEST_ADC_GetVoltage_mv(void)
{
    int raw, mv;
    adc_oneshot_read(adc_hdl, ADC_CHAN, &raw);
    adc_cali_raw_to_voltage(cal_hdl, raw, &mv);
    return mv;          // 直接返回 mV，避免 double
}