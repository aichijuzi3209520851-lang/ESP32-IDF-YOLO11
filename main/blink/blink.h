#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void blink_led(uint8_t s_led_state);
void configure_led(void);

#ifdef __cplusplus
}
#endif
