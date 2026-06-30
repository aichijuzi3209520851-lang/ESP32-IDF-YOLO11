#ifndef SHT20_H
#define SHT20_H

#include "esp_err.h"
#include <stdio.h>

#define WENDU_COMMAND      0xF3
#define SHIDU_COMMAND      0xF5

extern uint16_t STH20_WData;
extern uint16_t STH20_SData;
void Z_STH20_GetData(uint8_t command);
void sth20_app(void);
void sth20_init(void);
#endif