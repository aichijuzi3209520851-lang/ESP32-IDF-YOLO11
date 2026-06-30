#include <stdio.h>
#include <cstring>
#include <cstdlib>
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
#include "SHT20.h"
#include "TSL2584.h"
}

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
        int voltage_mv = TEST_ADC_GetVoltage_mv();

        snprintf(buf, sizeof(buf), "V:%dmV", voltage_mv);
        oled_draw_string(0, 32, buf);

        ESP_LOGI(TAG, "MAX471: %d mV", voltage_mv);
        vTaskDelay(pdMS_TO_TICKS(5200));
    }
}
static float randf(float lo, float hi) {
    return lo + (float)rand() / (float)RAND_MAX * (hi - lo);
}

static void oled_display_task(void *arg) {
    (void)arg;
    char buf[22];
    while (1) {
        float temp  = randf(15.0f, 45.0f);     // SHT20: 15~45°C
        float hum   = randf(30.0f, 90.0f);     // SHT20: 30~90%
        float lux   = randf(1000.0f, 100000.0f);// TSL2584: 1k~100k lux
        float curr  = randf(50.0f, 500.0f);     // MAX471: 50~500mA
        float volt  = randf(3.00f, 3.50f);      // MAX471: 3.0~3.5V

        oled_clear();
        oled_draw_string(0, 0, "ESP32-S3 CAM");

        snprintf(buf, sizeof(buf), "T:%.1fC H:%.0f%%", temp, hum);
        oled_draw_string(0, 16, buf);

        snprintf(buf, sizeof(buf), "Lux:%.0f", lux);
        oled_draw_string(0, 32, buf);

        snprintf(buf, sizeof(buf), "I:%.0fmA V:%.2fV", curr, volt);
        oled_draw_string(0, 48, buf);

        oled_update();
        vTaskDelay(pdMS_TO_TICKS(2000));
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

    //TEST_ADC_Init();
    //sth20_init();
    //tsl2584_init();

    //xTaskCreatePinnedToCore(sht20_task, "sht20", 3072, NULL, 5, NULL, 0);
    //xTaskCreatePinnedToCore(tsl2584_task, "tsl2584", 3072, NULL, 5, NULL, 0);
    //xTaskCreatePinnedToCore(max471_task, "max471", 3072, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(oled_display_task, "oled", 3072, NULL, 4, NULL, 0);

    start_stream_server();
    inference_task_start();

    while (1) {
        camera_fb_t *fb = camera_module_capture();
        if (fb) {
            uint8_t *jpg_copy = (uint8_t *)malloc(fb->len);
            if (jpg_copy) {
                memcpy(jpg_copy, fb->buf, fb->len);
                stream_update_frame(jpg_copy, fb->len);
                free(jpg_copy);
            }

            inference_task_send_frame(fb->buf, fb->len, fb->width, fb->height);

            camera_module_return(fb);
        } else {
            ESP_LOGW(TAG, "Capture failed");
        }
    }
}