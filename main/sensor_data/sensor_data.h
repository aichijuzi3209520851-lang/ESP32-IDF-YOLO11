#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float    temp;        /* 温度 °C */
    float    humi;        /* 湿度 %RH */
    uint32_t timestamp;   /* 最后更新时间 (ms since boot) */
    bool     valid;       /* 数据是否有效 */
} sensor_env_t;

/**
 * @brief 初始化传感器数据模块
 */
esp_err_t sensor_data_init(void);

/**
 * @brief 更新温湿度数据 (由 DHT11 任务调用)
 */
void sensor_data_update(float temp, float humi);

/**
 * @brief 标记当前温湿度数据无效
 */
void sensor_data_invalidate(void);

/**
 * @brief 获取最新温湿度数据的副本 (线程安全)
 */
sensor_env_t sensor_data_get(void);

#ifdef __cplusplus
}
#endif
