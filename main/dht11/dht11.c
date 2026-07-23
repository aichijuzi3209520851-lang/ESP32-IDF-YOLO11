#include "dht11.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "DHT11";
static portMUX_TYPE s_timing_mux = portMUX_INITIALIZER_UNLOCKED;

#define DHT11_START_LOW_US       20000
#define DHT11_RELEASE_US         30
#define DHT11_EDGE_TIMEOUT_US    120
#define DHT11_ONE_THRESHOLD_US   50

static int wait_until_level(gpio_num_t gpio_num, int level, int timeout_us) {
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(gpio_num) != level) {
        if ((esp_timer_get_time() - start) > timeout_us) return -1;
    }
    return (int)(esp_timer_get_time() - start);
}

static int measure_level(gpio_num_t gpio_num, int level, int timeout_us) {
    if (gpio_get_level(gpio_num) != level) return -1;

    int64_t start = esp_timer_get_time();
    while (gpio_get_level(gpio_num) == level) {
        if ((esp_timer_get_time() - start) > timeout_us) return -1;
    }
    return (int)(esp_timer_get_time() - start);
}

esp_err_t dht11_init(gpio_num_t gpio_num) {
    if (!GPIO_IS_VALID_OUTPUT_GPIO(gpio_num)) return ESP_ERR_INVALID_ARG;

    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << gpio_num,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) return err;
    return gpio_set_level(gpio_num, 1);
}

esp_err_t dht11_read(gpio_num_t gpio_num, float *temp, float *humi) {
    if (!temp || !humi || !GPIO_IS_VALID_OUTPUT_GPIO(gpio_num)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t data[5] = {0};
    esp_err_t result = ESP_OK;
    int failed_bit = -1;

    /* 主机起始信号：开漏拉低至少 18 ms，再释放总线。 */
    gpio_set_level(gpio_num, 0);
    esp_rom_delay_us(DHT11_START_LOW_US);

    /* 数据交换只有约 4 ms；锁住当前 CPU，避免任务切换破坏微秒时序。 */
    portENTER_CRITICAL(&s_timing_mux);
    gpio_set_level(gpio_num, 1);
    esp_rom_delay_us(DHT11_RELEASE_US);

    if (wait_until_level(gpio_num, 0, DHT11_EDGE_TIMEOUT_US) < 0 ||
        measure_level(gpio_num, 0, DHT11_EDGE_TIMEOUT_US) < 0 ||
        measure_level(gpio_num, 1, DHT11_EDGE_TIMEOUT_US) < 0) {
        result = ESP_ERR_TIMEOUT;
        goto timing_done;
    }

    for (int i = 0; i < 40; i++) {
        /* 每位先低约 50 us；真正区分 0/1 的是随后高电平的持续时间。 */
        if (measure_level(gpio_num, 0, DHT11_EDGE_TIMEOUT_US) < 0) {
            result = ESP_ERR_TIMEOUT;
            failed_bit = i;
            goto timing_done;
        }

        int high_us = measure_level(gpio_num, 1, DHT11_EDGE_TIMEOUT_US);
        if (high_us < 0) {
            result = ESP_ERR_TIMEOUT;
            failed_bit = i;
            goto timing_done;
        }

        data[i / 8] <<= 1;
        if (high_us > DHT11_ONE_THRESHOLD_US) data[i / 8] |= 1;
    }

timing_done:
    portEXIT_CRITICAL(&s_timing_mux);

    if (result != ESP_OK) {
        if (failed_bit >= 0) {
            ESP_LOGW(TAG, "Data timeout at bit %d", failed_bit);
        } else {
            ESP_LOGW(TAG, "Sensor response timeout");
        }
        return result;
    }

    uint8_t checksum = data[0] + data[1] + data[2] + data[3];
    if (checksum != data[4]) {
        ESP_LOGW(TAG, "Checksum mismatch: %02X != %02X", checksum, data[4]);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *humi = (float)data[0] + (float)data[1] / 10.0f;
    *temp = (float)(data[2] & 0x7f) + (float)data[3] / 10.0f;
    if (data[2] & 0x80) *temp = -*temp;

    return ESP_OK;
}
