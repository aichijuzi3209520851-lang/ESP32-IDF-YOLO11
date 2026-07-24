#include "led.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "LED";

void led_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_DEFECT_GPIO) | (1ULL << LED_NORMAL_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure LED GPIOs: %d", ret);
        return;
    }

    gpio_set_level(LED_DEFECT_GPIO, 0);
    gpio_set_level(LED_NORMAL_GPIO, 0);

    ESP_LOGI(TAG, "Status LEDs initialized: Defect=GPIO%d, Normal=GPIO%d",
             LED_DEFECT_GPIO, LED_NORMAL_GPIO);
}

void led_set_defect(bool has_defect)
{
    gpio_set_level(LED_DEFECT_GPIO, has_defect ? 1 : 0);
    gpio_set_level(LED_NORMAL_GPIO, has_defect ? 0 : 1);
}

void led_set_from_detection(int class_id)
{
    bool green_on = false;
    bool red_on = false;

    switch (class_id) {
        case YOLO_CLASS_STAIN:
            green_on = true;
            break;
        case YOLO_CLASS_DAMAGE:
            red_on = true;
            break;
        default:
            break;
    }

    gpio_set_level(LED_DEFECT_GPIO, red_on ? 1 : 0);
    gpio_set_level(LED_NORMAL_GPIO, green_on ? 1 : 0);

    ESP_LOGD(TAG, "Detection class %d -> LED: %s",
             class_id,
             red_on ? "RED" : (green_on ? "GREEN" : "OFF"));
}

void led_set_no_detection(void)
{
    gpio_set_level(LED_DEFECT_GPIO, 0);
    gpio_set_level(LED_NORMAL_GPIO, 0);
}
