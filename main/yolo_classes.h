#pragma once

#include <stdbool.h>

typedef enum {
    YOLO_CLASS_BIRD_DROP = 0,
    YOLO_CLASS_CLEAN = 1,
    YOLO_CLASS_DUST = 2,
    YOLO_CLASS_ELECTRICAL_DAMAGE = 3,
    YOLO_CLASS_PHYSICAL_DAMAGE = 4,
    YOLO_CLASS_SNOW_COVERED = 5,
    YOLO_CLASS_COUNT = 6,
} yolo_class_id_t;

#define YOLO_NUM_CLASSES YOLO_CLASS_COUNT

static inline float yolo_class_dirt_weight(int class_id)
{
    switch (class_id) {
        case YOLO_CLASS_BIRD_DROP:
            return 1.5f;
        case YOLO_CLASS_DUST:
            return 1.0f;
        default:
            return 0.0f;
    }
}

static inline int yolo_class_alert_priority(int class_id)
{
    switch (class_id) {
        case YOLO_CLASS_ELECTRICAL_DAMAGE:
        case YOLO_CLASS_PHYSICAL_DAMAGE:
            return 3;
        case YOLO_CLASS_SNOW_COVERED:
            return 2;
        case YOLO_CLASS_BIRD_DROP:
        case YOLO_CLASS_DUST:
            return 1;
        case YOLO_CLASS_CLEAN:
            return 0;
        default:
            return -1;
    }
}

static inline bool yolo_class_is_fault(int class_id)
{
    return class_id == YOLO_CLASS_ELECTRICAL_DAMAGE ||
           class_id == YOLO_CLASS_PHYSICAL_DAMAGE;
}

static inline bool yolo_class_blocks_wet_cleaning(int class_id)
{
    return yolo_class_is_fault(class_id) ||
           class_id == YOLO_CLASS_SNOW_COVERED;
}
