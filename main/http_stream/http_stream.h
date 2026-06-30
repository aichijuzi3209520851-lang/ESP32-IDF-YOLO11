#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t start_stream_server(void);

void stream_update_frame(const uint8_t *jpeg_data, size_t jpeg_len);

void stream_update_annotated(const uint8_t *jpeg_data, size_t jpeg_len);

#ifdef __cplusplus
}
#endif
