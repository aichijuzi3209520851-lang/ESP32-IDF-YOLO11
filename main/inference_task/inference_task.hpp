#pragma once

#include <stdint.h>
#include <stddef.h>

#define INFERENCE_STACK_SIZE   (64 * 1024)

typedef struct {
    uint8_t *jpeg_data;
    size_t   jpeg_len;
    int      width;
    int      height;
} inference_frame_t;

void inference_task_start(void);
bool inference_task_send_frame(const uint8_t *jpeg_data, size_t jpeg_len, int width, int height);