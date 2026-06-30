#include "TSL2584.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "I2C.h"

static const uint8_t CMD_ENABLE[]   = {0x80 | 0x00, 0x03};
static const uint8_t CMD_ATIME[]    = {0x80 | 0x01, 0xD5};
static const uint8_t CMD_GAIN[]     = {0x80 | 0x0F, 0x02};
static const uint8_t CMD_ID[]       = {0x80 | 0x12};
static const uint8_t CMD_CH0LOW = 0x80 | 0x14;

void tsl2584_init(void)
{
    uint8_t id = 0;
    uint8_t cmd_id = CMD_ID[0];

    ESP_ERROR_CHECK(i2c_master_transmit(tsl2584_dev, CMD_ENABLE, 2, 100));
    ESP_ERROR_CHECK(i2c_master_transmit_receive(tsl2584_dev, &cmd_id, 1, &id, 1, 100));
    printf("TSL2584 ID: 0x%02X\n", id);

    ESP_ERROR_CHECK(i2c_master_transmit(tsl2584_dev, CMD_ATIME, 2, 100));
    ESP_ERROR_CHECK(i2c_master_transmit(tsl2584_dev, CMD_GAIN,  2, 100));
}

int tsl2584_read_lux_x100(void)
{
    uint16_t ch0, ch1;
    uint8_t tx = CMD_CH0LOW;
    uint8_t rx[4];

    ESP_ERROR_CHECK(
        i2c_master_transmit_receive(tsl2584_dev, &tx, 1, rx, 4, 100));

    ch0 = (rx[1] << 8) | rx[0];
    ch1 = (rx[3] << 8) | rx[2];

    int lux100 = ((int32_t)ch0 - ch1) * 40800L / 1600;
    return lux100 < 0 ? 0 : lux100;
}