#pragma once

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 DHT11 单总线 GPIO
 *
 * @param[in] gpio_num DATA 引脚 GPIO 编号
 */
esp_err_t dht11_init(gpio_num_t gpio_num);

/**
 * @brief 读取 DHT11 传感器数据
 *
 * @param[in]  gpio_num DATA 引脚 GPIO 编号
 * @param[out] temp     温度值 (°C)
 * @param[out] humi     湿度值 (%RH)
 * @return ESP_OK 成功；ESP_ERR_INVALID_RESPONSE 校验失败；ESP_ERR_TIMEOUT 超时
 */
esp_err_t dht11_read(gpio_num_t gpio_num, float *temp, float *humi);

#ifdef __cplusplus
}
#endif
