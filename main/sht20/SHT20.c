#include <stdio.h>
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "I2C.h"

#define STH20_ADDR         0x40
#define WENDU_COMMAND      0xF3
#define SHIDU_COMMAND      0xF5
#define STHDU_WHO_I        0xE7
#define STHDU_FU_WEI       0XFE

uint16_t STH20_WData = 0;
uint16_t STH20_SData = 0;

void Z_STH20_GetData(uint8_t command) {
    uint8_t data[3];
    esp_err_t ret;

    ret = i2c_master_transmit(sht20_dev, &command, 1, 100);
    if (ret != ESP_OK) {
        printf("SHT20 send command failed: %d\n", ret);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(90));

    int count = 0;
    do {
        ret = i2c_master_receive(sht20_dev, data, 3, 100);
        if (ret == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    } while (++count < 10);

    if (count >= 10) {
        printf("SHT20 read timeout\n");
        return;
    }

    uint16_t raw_data = (data[0] << 8) | data[1];
    raw_data &= 0xFFFC;

    if (command == WENDU_COMMAND) {
        STH20_WData = raw_data;
    } else {
        STH20_SData = raw_data;
    }
}

void sth20_app(void) {
    Z_STH20_GetData(WENDU_COMMAND);
    Z_STH20_GetData(SHIDU_COMMAND);
    vTaskDelay(pdMS_TO_TICKS(1000));
}

void sth20_init(void) {
   STH20_SData = 0;
   STH20_WData = 0;
   uint8_t command = STHDU_WHO_I, command1 = STHDU_FU_WEI, reg;
   i2c_master_transmit(sht20_dev, &command1, 1, 100);
   vTaskDelay(pdMS_TO_TICKS(20));
   i2c_master_transmit_receive(sht20_dev, &command, 1, &reg, 1, 100);
   i2c_master_receive(sht20_dev, &reg, 1, 100);
}