#include "I2C.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "I2C";

i2c_master_bus_handle_t sensor_i2c_bus = NULL;
i2c_master_dev_handle_t sht20_dev = NULL;
i2c_master_dev_handle_t tsl2584_dev = NULL;

static void i2c_scan_bus(i2c_master_bus_handle_t bus)
{
    ESP_LOGI(TAG, "Scanning I2C bus for devices...");
    int found = 0;
    for (uint16_t addr = 1; addr < 127; addr++) {
        if (i2c_master_probe(bus, addr, 20) == ESP_OK) {
            ESP_LOGI(TAG, "  Device found at 0x%02X", addr);
            found++;
        }
    }
    if (found == 0) {
        ESP_LOGW(TAG, "  No devices found on I2C bus");
    }
    ESP_LOGI(TAG, "Scan complete, %d device(s) found", found);
}

esp_err_t sensor_i2c_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = GPIO_NUM_2,
        .scl_io_num = GPIO_NUM_3,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    ESP_RETURN_ON_ERROR(
        i2c_new_master_bus(&bus_cfg, &sensor_i2c_bus),
        TAG, "Failed to create I2C bus"
    );

    i2c_scan_bus(sensor_i2c_bus);

    i2c_device_config_t sht20_cfg = {
        .device_address = 0x40,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(
        i2c_master_bus_add_device(sensor_i2c_bus, &sht20_cfg, &sht20_dev),
        TAG, "Failed to add SHT20"
    );

    i2c_device_config_t tsl2584_cfg = {
        .device_address = 0x39,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(
        i2c_master_bus_add_device(sensor_i2c_bus, &tsl2584_cfg, &tsl2584_dev),
        TAG, "Failed to add TSL2584"
    );

    // OLED is handled by chill-sam/ssd1306 component via ssd1306_new_i2c()
    ESP_LOGI(TAG, "I2C bus (NUM_0) initialized: SHT20(0x40) TSL2584(0x39)");
    return ESP_OK;
}