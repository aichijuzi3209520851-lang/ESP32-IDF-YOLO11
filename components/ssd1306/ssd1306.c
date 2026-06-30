#include "ssd1306.h"

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define SSD1306_CTRL_CMD 0x00
#define SSD1306_CTRL_DATA 0x40
#define SSD1306_FB_LEN(w, h) ((size_t)((w) * (h) / 8))

static const char *TAG = "SSD1306";

struct ssd1306_t {
    i2c_master_dev_handle_t dev;
    uint8_t *fb;
    size_t fb_len;
    uint16_t width;
    uint16_t height;
    bool owns_fb;
};

static esp_err_t ssd1306_send_cmd(ssd1306_handle_t h, const uint8_t *cmds,
                                  size_t len)
{
    uint8_t buf[17];
    size_t off = 0;

    while (off < len) {
        size_t chunk = len - off;
        if (chunk > sizeof(buf) - 1) {
            chunk = sizeof(buf) - 1;
        }

        buf[0] = SSD1306_CTRL_CMD;
        memcpy(&buf[1], &cmds[off], chunk);
        ESP_RETURN_ON_ERROR(i2c_master_transmit(h->dev, buf, chunk + 1, -1),
                            TAG, "send command");
        off += chunk;
    }

    return ESP_OK;
}

static esp_err_t ssd1306_send_data(ssd1306_handle_t h, const uint8_t *data,
                                   size_t len)
{
    uint8_t buf[33];
    size_t off = 0;

    while (off < len) {
        size_t chunk = len - off;
        if (chunk > sizeof(buf) - 1) {
            chunk = sizeof(buf) - 1;
        }

        buf[0] = SSD1306_CTRL_DATA;
        memcpy(&buf[1], &data[off], chunk);
        ESP_RETURN_ON_ERROR(i2c_master_transmit(h->dev, buf, chunk + 1, -1),
                            TAG, "send data");
        off += chunk;
    }

    return ESP_OK;
}

static esp_err_t ssd1306_reset(gpio_num_t rst_gpio)
{
    if (rst_gpio == GPIO_NUM_NC) {
        return ESP_OK;
    }

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << rst_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "reset gpio config");

    gpio_set_level(rst_gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(rst_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

static const uint8_t *glyph5x7(char ch)
{
    static const uint8_t blank[5] = {0x00, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t glyphs[][5] = {
        {0x3e, 0x51, 0x49, 0x45, 0x3e}, // 0
        {0x00, 0x42, 0x7f, 0x40, 0x00}, // 1
        {0x42, 0x61, 0x51, 0x49, 0x46}, // 2
        {0x21, 0x41, 0x45, 0x4b, 0x31}, // 3
        {0x18, 0x14, 0x12, 0x7f, 0x10}, // 4
        {0x27, 0x45, 0x45, 0x45, 0x39}, // 5
        {0x3c, 0x4a, 0x49, 0x49, 0x30}, // 6
        {0x01, 0x71, 0x09, 0x05, 0x03}, // 7
        {0x36, 0x49, 0x49, 0x49, 0x36}, // 8
        {0x06, 0x49, 0x49, 0x29, 0x1e}, // 9
    };
    static const uint8_t alpha[][5] = {
        {0x7e, 0x11, 0x11, 0x11, 0x7e}, // A
        {0x7f, 0x49, 0x49, 0x49, 0x36}, // B
        {0x3e, 0x41, 0x41, 0x41, 0x22}, // C
        {0x7f, 0x41, 0x41, 0x22, 0x1c}, // D
        {0x7f, 0x49, 0x49, 0x49, 0x41}, // E
        {0x7f, 0x09, 0x09, 0x09, 0x01}, // F
        {0x3e, 0x41, 0x49, 0x49, 0x7a}, // G
        {0x7f, 0x08, 0x08, 0x08, 0x7f}, // H
        {0x00, 0x41, 0x7f, 0x41, 0x00}, // I
        {0x20, 0x40, 0x41, 0x3f, 0x01}, // J
        {0x7f, 0x08, 0x14, 0x22, 0x41}, // K
        {0x7f, 0x40, 0x40, 0x40, 0x40}, // L
        {0x7f, 0x02, 0x0c, 0x02, 0x7f}, // M
        {0x7f, 0x04, 0x08, 0x10, 0x7f}, // N
        {0x3e, 0x41, 0x41, 0x41, 0x3e}, // O
        {0x7f, 0x09, 0x09, 0x09, 0x06}, // P
        {0x3e, 0x41, 0x51, 0x21, 0x5e}, // Q
        {0x7f, 0x09, 0x19, 0x29, 0x46}, // R
        {0x46, 0x49, 0x49, 0x49, 0x31}, // S
        {0x01, 0x01, 0x7f, 0x01, 0x01}, // T
        {0x3f, 0x40, 0x40, 0x40, 0x3f}, // U
        {0x1f, 0x20, 0x40, 0x20, 0x1f}, // V
        {0x3f, 0x40, 0x38, 0x40, 0x3f}, // W
        {0x63, 0x14, 0x08, 0x14, 0x63}, // X
        {0x07, 0x08, 0x70, 0x08, 0x07}, // Y
        {0x61, 0x51, 0x49, 0x45, 0x43}, // Z
    };
    static const uint8_t period[5] = {0x00, 0x60, 0x60, 0x00, 0x00};
    static const uint8_t colon[5] = {0x00, 0x36, 0x36, 0x00, 0x00};
    static const uint8_t percent[5] = {0x23, 0x13, 0x08, 0x64, 0x62};
    static const uint8_t dash[5] = {0x08, 0x08, 0x08, 0x08, 0x08};
    static const uint8_t slash[5] = {0x20, 0x10, 0x08, 0x04, 0x02};

    if (ch >= '0' && ch <= '9') {
        return glyphs[ch - '0'];
    }

    ch = (char)toupper((unsigned char)ch);
    if (ch >= 'A' && ch <= 'Z') {
        return alpha[ch - 'A'];
    }
    if (ch == '.') {
        return period;
    }
    if (ch == ':') {
        return colon;
    }
    if (ch == '%') {
        return percent;
    }
    if (ch == '-' || ch == '_') {
        return dash;
    }
    if (ch == '/') {
        return slash;
    }
    return blank;
}

esp_err_t ssd1306_new_i2c(const ssd1306_config_t *cfg, ssd1306_handle_t *out)
{
    ESP_RETURN_ON_FALSE(cfg && out, ESP_ERR_INVALID_ARG, TAG, "invalid args");
    ESP_RETURN_ON_FALSE(cfg->bus == SSD1306_I2C, ESP_ERR_NOT_SUPPORTED, TAG,
                        "only I2C is supported");
    ESP_RETURN_ON_FALSE(cfg->width && cfg->height, ESP_ERR_INVALID_ARG, TAG,
                        "invalid size");

    i2c_master_bus_handle_t bus = NULL;
    ESP_RETURN_ON_ERROR(i2c_master_get_bus_handle(cfg->iface.i2c.port, &bus),
                        TAG, "I2C bus not initialized");

    ssd1306_handle_t h = calloc(1, sizeof(*h));
    ESP_RETURN_ON_FALSE(h, ESP_ERR_NO_MEM, TAG, "no memory");

    h->width = cfg->width;
    h->height = cfg->height;
    h->fb_len = SSD1306_FB_LEN(cfg->width, cfg->height);

    if (cfg->fb) {
        if (cfg->fb_len != h->fb_len) {
            free(h);
            return ESP_ERR_INVALID_SIZE;
        }
        h->fb = cfg->fb;
    } else {
        h->fb = calloc(1, h->fb_len);
        h->owns_fb = true;
    }

    if (!h->fb) {
        free(h);
        return ESP_ERR_NO_MEM;
    }

    i2c_device_config_t dev_cfg = {
        .device_address = cfg->iface.i2c.addr,
        .scl_speed_hz = 400000,
    };

    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_cfg, &h->dev);
    if (ret != ESP_OK) {
        ssd1306_del(h);
        return ret;
    }

    ret = ssd1306_reset(cfg->iface.i2c.rst_gpio);
    if (ret != ESP_OK) {
        ssd1306_del(h);
        return ret;
    }

    const uint8_t init[] = {
        0xae, 0x20, 0x00, 0xa8, (uint8_t)(h->height - 1), 0xd3, 0x00,
        0x40, 0xa1, 0xc8, 0xda, 0x12, 0x81, 0x7f, 0xa4, 0xa6,
        0xd5, 0x80, 0xd9, 0xf1, 0xdb, 0x40, 0x8d, 0x14, 0xaf,
    };

    ret = ssd1306_send_cmd(h, init, sizeof(init));
    if (ret != ESP_OK) {
        ssd1306_del(h);
        return ret;
    }

    *out = h;
    return ESP_OK;
}

esp_err_t ssd1306_clear(ssd1306_handle_t h)
{
    ESP_RETURN_ON_FALSE(h && h->fb, ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    memset(h->fb, 0, h->fb_len);
    return ESP_OK;
}

esp_err_t ssd1306_draw_pixel(ssd1306_handle_t h, int x, int y, bool on)
{
    ESP_RETURN_ON_FALSE(h && h->fb, ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    if (x < 0 || y < 0 || x >= h->width || y >= h->height) {
        return ESP_OK;
    }

    size_t idx = (size_t)(y / 8) * h->width + x;
    uint8_t mask = (uint8_t)(1U << (y & 7));
    if (on) {
        h->fb[idx] |= mask;
    } else {
        h->fb[idx] &= (uint8_t)~mask;
    }
    return ESP_OK;
}

static void draw_char(ssd1306_handle_t h, int x, int y, char ch, bool on)
{
    const uint8_t *glyph = glyph5x7(ch);

    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; row++) {
            if (bits & (1U << row)) {
                (void)ssd1306_draw_pixel(h, x + col, y + row, on);
            }
        }
    }
}

esp_err_t ssd1306_draw_text_wrapped(ssd1306_handle_t h, int x, int y, int w,
                                    int hgt, const char *text, bool on)
{
    ESP_RETURN_ON_FALSE(h && text, ESP_ERR_INVALID_ARG, TAG, "invalid args");

    int cx = x;
    int cy = y;
    const int char_w = 6;
    const int line_h = 8;

    for (const char *p = text; *p && cy + 7 < y + hgt; p++) {
        if (*p == '\n' || cx + 5 >= x + w) {
            cx = x;
            cy += line_h;
            if (*p == '\n') {
                continue;
            }
        }

        if (cy + 7 >= y + hgt) {
            break;
        }

        draw_char(h, cx, cy, *p, on);
        cx += char_w;
    }

    return ESP_OK;
}

esp_err_t ssd1306_display(ssd1306_handle_t h)
{
    ESP_RETURN_ON_FALSE(h && h->fb, ESP_ERR_INVALID_ARG, TAG, "invalid handle");

    const uint8_t window[] = {
        0x21, 0x00, (uint8_t)(h->width - 1),
        0x22, 0x00, (uint8_t)((h->height / 8) - 1),
    };
    ESP_RETURN_ON_ERROR(ssd1306_send_cmd(h, window, sizeof(window)), TAG,
                        "set window");
    return ssd1306_send_data(h, h->fb, h->fb_len);
}

esp_err_t ssd1306_del(ssd1306_handle_t h)
{
    if (!h) {
        return ESP_OK;
    }

    if (h->dev) {
        (void)i2c_master_bus_rm_device(h->dev);
    }
    if (h->owns_fb) {
        free(h->fb);
    }
    free(h);
    return ESP_OK;
}
