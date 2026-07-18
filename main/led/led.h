#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "yolo_classes.h"

#define LED_DEFECT_GPIO   14
#define LED_NORMAL_GPIO   19

#ifdef __cplusplus
extern "C" {
#endif

void led_init(void);

void led_set_defect(bool has_defect);

void led_set_from_detection(int class_id);

void led_set_no_detection(void);

#ifdef __cplusplus
}
#endif
