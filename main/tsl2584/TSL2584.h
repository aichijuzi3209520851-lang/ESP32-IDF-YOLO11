#ifndef TSL2584_H
#define TSL2584_H

#include "esp_err.h"
#include <stdlib.h>

void tsl2584_init(void);
int tsl2584_read_lux_x100(void);

#endif