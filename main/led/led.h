#pragma once

#include <stdint.h>
#include <stdbool.h>

#define LED_DEFECT_GPIO   14
#define LED_NORMAL_GPIO   19

#define DEFECT_CLASS_BIRD_DROP       0
#define DEFECT_CLASS_CLEAN           1
#define DEFECT_CLASS_DUST            2
#define DEFECT_CLASS_ELECTRICAL      3
#define DEFECT_CLASS_PHYSICAL        4
#define DEFECT_CLASS_SNOW            5

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
