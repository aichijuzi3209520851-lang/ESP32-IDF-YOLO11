#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define INFERENCE_STACK_SIZE   (64 * 1024)

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *jpeg_data;
    size_t   jpeg_len;
    int      width;
    int      height;
} inference_frame_t;

typedef struct {
    bool ready;
    uint32_t sequence;
    uint32_t latency_ms;
    float fps;
    uint16_t detections;
    bool stain_detected;
    bool damage_detected;
    int top_class_id;
    float top_score;
    char top_class[24];
} inference_status_t;

void inference_task_start(void);
bool inference_task_send_frame(const uint8_t *jpeg_data, size_t jpeg_len, int width, int height);
bool inference_status_get(inference_status_t *status);

#ifdef __cplusplus
}
#endif
