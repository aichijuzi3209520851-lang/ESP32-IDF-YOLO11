#pragma once

#include <ssd1306.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize the SSD1306 OLED display via I2C (call after sensor_i2c_init). */
esp_err_t oled_init(void);

/** Clear the display framebuffer. */
void oled_clear(void);

/** Draw a string at (x, y). Wraps if text exceeds display width. */
void oled_draw_string(int x, int y, const char *str);

/** Flush framebuffer to the physical display. */
void oled_update(void);

#ifdef __cplusplus
}
#endif
