#include <stdio.h>
#include <cstring>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "camera.h"
#include "wifi.h"
#include "http_stream.h"
#include "blink.h"
#include "inference_task.hpp"
#include "led.h"
#include "I2C.h"
#include "oled/oled.h"
#include "img_converters.h"

extern "C" {
#include "MAX471.h"
#include "LightSensor.h"
#include "SHT20.h"
#include "TSL2584.h"
#include "dht11.h"
#include "sensor_data.h"
}

#define DHT11_GPIO GPIO_NUM_38

static const char *TAG = "CAM_APP";

static void sht20_task(void *arg) {
    (void)arg;
    char buf[22];
    while (1) {
        Z_STH20_GetData(WENDU_COMMAND);
        int16_t temp_raw = STH20_WData;
        Z_STH20_GetData(SHIDU_COMMAND);
        int16_t hum_raw = STH20_SData;

        float temp_c = -46.85 + 175.72 * (float)temp_raw / 65536.0f;
        float hum_pct = -6.0 + 125.0 * (float)hum_raw / 65536.0f;

        snprintf(buf, sizeof(buf), "T:%.1fC H:%.0f%%", temp_c, hum_pct);
        oled_draw_string(0, 0, buf);

        ESP_LOGI(TAG, "SHT20: %.1fC %.0f%%", temp_c, hum_pct);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void tsl2584_task(void *arg) {
    (void)arg;
    char buf[22];
    while (1) {
        uint32_t lux_x100 = tsl2584_read_lux_x100();
        float lux = (float)lux_x100 / 100.0f;

        snprintf(buf, sizeof(buf), "Lux:%.0f", lux);
        oled_draw_string(0, 16, buf);

        ESP_LOGI(TAG, "TSL2584: %.0f lux", lux);
        vTaskDelay(pdMS_TO_TICKS(5100));
    }
}

static void max471_task(void *arg) {
    (void)arg;
    char buf[22];
    while (1) {
        float current_ma = 0.0f;
        esp_err_t err = max471_read_current_ma(&current_ma);

        if (err == ESP_OK) {
            snprintf(buf, sizeof(buf), "I:%.1fmA", current_ma);
            ESP_LOGI(TAG, "MAX471: %.1f mA", current_ma);
        } else {
            snprintf(buf, sizeof(buf), "I:----mA");
            ESP_LOGW(TAG, "MAX471 read failed: %s", esp_err_to_name(err));
        }
        oled_draw_string(0, 32, buf);

        vTaskDelay(pdMS_TO_TICKS(5200));
    }
}
static void dht11_task(void *arg) {
    (void)arg;
    float temp, humi;
    while (1) {
        esp_err_t err = dht11_read(DHT11_GPIO, &temp, &humi);
        if (err == ESP_OK) {
            sensor_data_update(temp, humi);
            ESP_LOGI(TAG, "DHT11: %.1fC %.1f%%", temp, humi);
        } else {
            sensor_data_invalidate();
            ESP_LOGW(TAG, "DHT11 read failed: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

static void oled_display_task(void *arg) {
    (void)arg;
    char buf[22];
    while (1) {
        sensor_env_t env = sensor_data_get();
        light_sensor_data_t light = {};
        esp_err_t light_err = light_sensor_read(&light);
        float current_ma = 0.0f;
        esp_err_t max471_err = max471_read_current_ma(&current_ma);
        bool current_valid = max471_err == ESP_OK;
        if (max471_err != ESP_OK) {
            ESP_LOGW(TAG, "MAX471 read failed: %s", esp_err_to_name(max471_err));
        }
        if (light_err == ESP_OK) {
            ESP_LOGI(TAG, "Light: raw=%d, %d mV, %.1f%%",
                     light.raw, light.voltage_mv, light.intensity_percent);
        } else {
            ESP_LOGW(TAG, "Light sensor read failed: %s", esp_err_to_name(light_err));
        }

        oled_clear();
        oled_draw_string(0, 0, "ESP32-S3 CAM");

        if (env.valid) {
            snprintf(buf, sizeof(buf), "T:%.1fC H:%.0f%%", env.temp, env.humi);
        } else {
            snprintf(buf, sizeof(buf), "T:--.-C H:--%%");
        }
        oled_draw_string(0, 16, buf);

        if (light_err == ESP_OK) {
            snprintf(buf, sizeof(buf), "Light:%.0f%%", light.intensity_percent);
        } else {
            snprintf(buf, sizeof(buf), "Light:---%%");
        }
        oled_draw_string(0, 32, buf);

        if (current_valid) {
            snprintf(buf, sizeof(buf), "I:%.1fmA", current_ma);
        } else {
            snprintf(buf, sizeof(buf), "I:----mA");
        }
        oled_draw_string(0, 48, buf);

        oled_update();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
static void capture_task(void *arg) {
    (void)arg;
    while (1) {
        camera_fb_t *fb = camera_module_capture();
        if (fb) {
            stream_update_frame(fb->buf, fb->len);
            inference_task_send_frame(fb->buf, fb->len, fb->width, fb->height);
            camera_module_return(fb);
        } else {
            ESP_LOGW(TAG, "Capture failed");
        }
        // 短暂让出 CPU，保证同核的 HTTP 服务器有时间片发送 MJPEG 流
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

extern "C" void app_main(void) {

    ESP_LOGI(TAG, "App starting on core %d", xPortGetCoreID());
    ESP_ERROR_CHECK(nvs_flash_init());

    wifi_init_sta();

    ESP_LOGI(TAG, "Initializing camera...");
    if (camera_module_init() != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed");
        return;
    }

    ESP_LOGI(TAG, "Initializing LED...");
    configure_led();

    ESP_LOGI(TAG, "Initializing LEDs...");
    led_init();

    ESP_ERROR_CHECK(sensor_i2c_init());
    oled_init();
    ESP_ERROR_CHECK(sensor_data_init());
    ESP_ERROR_CHECK(dht11_init(DHT11_GPIO));
    ESP_ERROR_CHECK(max471_init());
    ESP_ERROR_CHECK(light_sensor_init());

    //TEST_ADC_Init();
    //sth20_init();
    //tsl2584_init();

    //xTaskCreatePinnedToCore(sht20_task, "sht20", 3072, NULL, 5, NULL, 0);
    //xTaskCreatePinnedToCore(tsl2584_task, "tsl2584", 3072, NULL, 5, NULL, 0);
    //xTaskCreatePinnedToCore(max471_task, "max471", 3072, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(dht11_task, "dht11", 3072, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(oled_display_task, "oled", 3072, NULL, 4, NULL, 0);

    start_stream_server();
    inference_task_start();

    // 采集任务放到 Core 0，优先级高于 HTTP 服务器，但通过 vTaskDelay 让出时间片
    xTaskCreatePinnedToCore(capture_task, "capture", 4096, NULL, 6, NULL, 0);

    // app_main 不返回 — 主任务保持存活，优先级最低，不影响其他任务
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
