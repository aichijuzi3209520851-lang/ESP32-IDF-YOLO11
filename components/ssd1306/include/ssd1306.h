#pragma once

#include <driver/gpio.h>
#include <driver/i2c_types.h>
#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SSD1306_I2C = 0,
    SSD1306_SPI = 1,
} ssd1306_bus_t;

typedef struct {
    i2c_port_num_t port;
    gpio_num_t rst_gpio;
    uint8_t addr;
} ssd1306_i2c_cfg_t;

typedef struct {
    int host;
    int cs_gpio;
    int dc_gpio;
    int rst_gpio;
    int clk_hz;
} ssd1306_spi_cfg_t;

typedef struct {
    union {
        ssd1306_i2c_cfg_t i2c;
        ssd1306_spi_cfg_t spi;
    } iface;

    uint8_t *fb;
    size_t fb_len;
    ssd1306_bus_t bus;
    uint16_t width;
    uint16_t height;
} ssd1306_config_t;

typedef struct ssd1306_t *ssd1306_handle_t;

esp_err_t ssd1306_new_i2c(const ssd1306_config_t *cfg, ssd1306_handle_t *out);
esp_err_t ssd1306_clear(ssd1306_handle_t h);
esp_err_t ssd1306_draw_pixel(ssd1306_handle_t h, int x, int y, bool on);
esp_err_t ssd1306_draw_text_wrapped(ssd1306_handle_t h, int x, int y, int w,
                                    int hgt, const char *text, bool on);
esp_err_t ssd1306_display(ssd1306_handle_t h);
esp_err_t ssd1306_del(ssd1306_handle_t h);

#ifdef __cplusplus
}
#endif
