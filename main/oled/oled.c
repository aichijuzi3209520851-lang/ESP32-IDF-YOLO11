#include "oled.h"
#include "esp_log.h"
#include <driver/gpio.h>

static const char *TAG = "OLED";
static ssd1306_handle_t g_oled = NULL;

esp_err_t oled_init(void)
{
    ssd1306_config_t cfg = {
        .width  = 128,
        .height = 64,
        .fb     = NULL,
        .fb_len = 0,
        .bus    = SSD1306_I2C,
        .iface.i2c = {
            .port     = I2C_NUM_0,
            .addr     = 0x3C,
            .rst_gpio = GPIO_NUM_NC,
        },
    };

    esp_err_t ret = ssd1306_new_i2c(&cfg, &g_oled);
    if (ret != ESP_OK) {
        // Try alternate address
        cfg.iface.i2c.addr = 0x3D;
        ret = ssd1306_new_i2c(&cfg, &g_oled);
    }

    if (ret == ESP_OK) {
        ssd1306_clear(g_oled);
        ssd1306_display(g_oled);
        ESP_LOGI(TAG, "OLED initialized at 0x%02X", cfg.iface.i2c.addr);
    } else {
        ESP_LOGE(TAG, "OLED init failed");
    }
    return ret;
}

void oled_clear(void)
{
    if (g_oled) ssd1306_clear(g_oled);
}

void oled_draw_string(int x, int y, const char *str)
{
    if (g_oled) ssd1306_draw_text_wrapped(g_oled, x, y, 128 - x, 64 - y, str, true);
}

void oled_update(void)
{
    if (g_oled) ssd1306_display(g_oled);
}
