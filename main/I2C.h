#ifndef I2C_H
#define I2C_H

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

extern i2c_master_bus_handle_t sensor_i2c_bus;
extern i2c_master_dev_handle_t sht20_dev;
extern i2c_master_dev_handle_t tsl2584_dev;

esp_err_t sensor_i2c_init(void);

#ifdef __cplusplus
}
#endif

#endif