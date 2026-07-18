#pragma once

#include <stdint.h>
#include <vector>
#include "esp_err.h"
#include "yolo_classes.h"

#define YOLO_IMGSZ 320
#define YOLO_CONF_THRESHOLD 0.10f
#define YOLO_IOU_THRESHOLD 0.50f
#define YOLO_MAX_DETECTIONS 100

typedef struct {
    int class_id;
    float score;
    int x1, y1, x2, y2;
} detection_t;

class YOLODetector {
public:
    YOLODetector();
    ~YOLODetector();

    esp_err_t init();

    std::vector<detection_t> detect(
        const uint8_t *rgb888_data,
        int img_width, int img_height
    );

    uint8_t* annotate_jpeg(
        const uint8_t *rgb888_data,
        int img_width, int img_height,
        const std::vector<detection_t> &results,
        size_t *out_len
    );

    void print_results(const std::vector<detection_t> &results);
    const char* class_name(int class_id) const;

private:
    void *model_ = nullptr;
    float scale_x_ = 1.0f;
    float scale_y_ = 1.0f;
    int pad_w_ = 0;
    int pad_h_ = 0;
};
