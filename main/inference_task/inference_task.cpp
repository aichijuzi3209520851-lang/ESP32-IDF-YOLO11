#include "inference_task.hpp"
#include "yolo_detect.hpp"
#include "http_stream.h"
#include "led.h"
#include "img_converters.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <cstring>
#include <cstdlib>

static const char *TAG = "INFERENCE";

static YOLODetector *s_detector = NULL;
static inference_frame_t s_latest_frame = {};
static SemaphoreHandle_t s_frame_mtx = NULL;
static SemaphoreHandle_t s_frame_sem = NULL;
static volatile bool s_inference_ready = true;

static void inference_task_run(void *arg) {
    ESP_LOGI(TAG, "Inference task started on core %d", xPortGetCoreID());

    esp_task_wdt_add(NULL);
    esp_task_wdt_reset();

    s_detector = new YOLODetector();
    if (s_detector->init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize detector");
        delete s_detector;
        s_detector = NULL;
        esp_task_wdt_delete(NULL);
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        s_inference_ready = true;
        xSemaphoreTake(s_frame_sem, portMAX_DELAY);
        s_inference_ready = false;
        esp_task_wdt_reset();

        inference_frame_t frame;
        xSemaphoreTake(s_frame_mtx, portMAX_DELAY);
        frame = s_latest_frame;
        s_latest_frame.jpeg_data = NULL;
        s_latest_frame.jpeg_len = 0;
        xSemaphoreGive(s_frame_mtx);

        if (!frame.jpeg_data) continue;

        size_t rgb888_size = frame.width * frame.height * 3;
        uint8_t *rgb888_buf = (uint8_t *)heap_caps_malloc(rgb888_size, MALLOC_CAP_SPIRAM);
        if (!rgb888_buf) {
            rgb888_buf = (uint8_t *)malloc(rgb888_size);
        }
        if (!rgb888_buf) {
            ESP_LOGE(TAG, "Failed to alloc RGB888 buffer (%zu bytes)", rgb888_size);
            free(frame.jpeg_data);
            continue;
        }

        bool dec_ok = fmt2rgb888(frame.jpeg_data, frame.jpeg_len,
                                 PIXFORMAT_JPEG, rgb888_buf);
        if (!dec_ok) {
            ESP_LOGE(TAG, "JPEG decode failed");
            heap_caps_free(rgb888_buf);
            free(frame.jpeg_data);
            continue;
        }

        auto results = s_detector->detect(
            rgb888_buf, frame.width, frame.height
        );

        if (!results.empty()) {
            int worst_class = results[0].class_id;
            led_set_from_detection(worst_class);
        } else {
            led_set_no_detection();
        }

        size_t annotated_len = 0;
        uint8_t *annotated_jpeg = NULL;

        if (!results.empty()) {
            s_detector->print_results(results);

            annotated_jpeg = s_detector->annotate_jpeg(
                rgb888_buf, frame.width, frame.height,
                results, &annotated_len
            );
        }

        if (!annotated_jpeg) {
            annotated_jpeg = (uint8_t *)malloc(frame.jpeg_len);
            if (annotated_jpeg) {
                memcpy(annotated_jpeg, frame.jpeg_data, frame.jpeg_len);
                annotated_len = frame.jpeg_len;
            }
        }

        if (annotated_jpeg) {
            ESP_LOGI(TAG, "Annotated JPEG: %zu bytes, detections: %s",
                     annotated_len, results.empty() ? "NO" : "YES");

            stream_update_annotated(annotated_jpeg, annotated_len);
            free(annotated_jpeg);
        }

        heap_caps_free(rgb888_buf);
        free(frame.jpeg_data);
        esp_task_wdt_reset();
    }
}

void inference_task_start(void) {
    s_frame_mtx = xSemaphoreCreateMutex();
    s_frame_sem = xSemaphoreCreateBinary();
    if (!s_frame_mtx || !s_frame_sem) {
        ESP_LOGE(TAG, "Failed to create frame sync primitives");
        return;
    }

    TaskHandle_t task_handle;
    xTaskCreatePinnedToCore(
        inference_task_run,
        "inference",
        INFERENCE_STACK_SIZE,
        NULL,
        5,
        &task_handle,
        1
    );
    ESP_LOGI(TAG, "Inference task created on core 1");
}

bool inference_task_send_frame(const uint8_t *jpeg_data, size_t jpeg_len, int width, int height) {
    if (!s_frame_sem) return false;
    if (!s_inference_ready) return true;

    uint8_t *new_buf = (uint8_t *)malloc(jpeg_len);
    if (!new_buf) return false;
    memcpy(new_buf, jpeg_data, jpeg_len);

    uint8_t *old_buf = NULL;

    xSemaphoreTake(s_frame_mtx, portMAX_DELAY);
    old_buf = s_latest_frame.jpeg_data;
    s_latest_frame.jpeg_data = new_buf;
    s_latest_frame.jpeg_len = jpeg_len;
    s_latest_frame.width = width;
    s_latest_frame.height = height;
    xSemaphoreGive(s_frame_mtx);

    if (old_buf) free(old_buf);

    xSemaphoreGive(s_frame_sem);
    return true;
}
