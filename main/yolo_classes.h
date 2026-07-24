#pragma once

#include <stdbool.h>

typedef enum {
    YOLO_CLASS_STAIN = 0,
    YOLO_CLASS_DAMAGE = 1,
    YOLO_CLASS_COUNT = 2,
} yolo_class_id_t;

#define YOLO_NUM_CLASSES YOLO_CLASS_COUNT

static inline float yolo_class_dirt_weight(int class_id)
{
    return class_id == YOLO_CLASS_STAIN ? 1.0f : 0.0f;
}

static inline int yolo_class_alert_priority(int class_id)
{
    switch (class_id) {
        case YOLO_CLASS_DAMAGE:
            return 3;
        case YOLO_CLASS_STAIN:
            return 1;
        default:
            return -1;
    }
}

static inline bool yolo_class_is_fault(int class_id)
{
    return class_id == YOLO_CLASS_DAMAGE;
}

static inline bool yolo_class_blocks_wet_cleaning(int class_id)
{
    return yolo_class_is_fault(class_id);
}
