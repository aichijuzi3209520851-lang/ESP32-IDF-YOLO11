#include "inference_task.hpp"
#include "yolo_detect.hpp"
#include "http_stream.h"
#include "led.h"
#include "img_converters.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>

static const char *TAG = "INFERENCE";

static YOLODetector *s_detector = NULL;
static inference_frame_t s_latest_frame = {};
static SemaphoreHandle_t s_frame_mtx = NULL;
static SemaphoreHandle_t s_frame_sem = NULL;
static SemaphoreHandle_t s_status_mtx = NULL;
static volatile bool s_inference_ready = true;
static inference_status_t s_status = {};
static int64_t s_last_completed_us = 0;

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

        int64_t inference_start_us = esp_timer_get_time();
        auto results = s_detector->detect(
            rgb888_buf, frame.width, frame.height
        );
        int64_t inference_end_us = esp_timer_get_time();

        if (!results.empty()) {
            int highest_priority_class = -1;
            int highest_priority = -1;
            float weighted_dirty_area = 0.0f;
            bool has_fault = false;
            bool wet_cleaning_blocked = false;

            for (const auto &result : results) {
                int priority = yolo_class_alert_priority(result.class_id);
                if (priority > highest_priority) {
                    highest_priority = priority;
                    highest_priority_class = result.class_id;
                }

                int box_width = result.x2 - result.x1;
                int box_height = result.y2 - result.y1;
                if (box_width > 0 && box_height > 0) {
                    weighted_dirty_area += (float)(box_width * box_height) *
                                           yolo_class_dirt_weight(result.class_id);
                }
                has_fault = has_fault || yolo_class_is_fault(result.class_id);
                wet_cleaning_blocked = wet_cleaning_blocked ||
                                       yolo_class_blocks_wet_cleaning(result.class_id);
            }

            float image_area = (float)(frame.width * frame.height);
            float dirt_index = image_area > 0.0f
                                   ? weighted_dirty_area / image_area * 100.0f
                                   : 0.0f;
            if (dirt_index > 100.0f) dirt_index = 100.0f;

            led_set_from_detection(highest_priority_class);
            ESP_LOGI(TAG, "Vision state: dirt=%.1f%% fault=%s wet_clean=%s",
                     dirt_index,
                     has_fault ? "YES" : "NO",
                     wet_cleaning_blocked ? "BLOCKED" : "ALLOWED");
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

        inference_status_t next = {};
        next.ready = true;
        next.latency_ms = (uint32_t)((inference_end_us - inference_start_us) / 1000);
        next.fps = s_last_completed_us > 0
                       ? 1000000.0f / (float)(inference_end_us - s_last_completed_us)
                       : 0.0f;
        next.detections = (uint16_t)results.size();
        next.top_class_id = -1;

        for (const auto &result : results) {
            next.stain_detected = next.stain_detected || result.class_id == YOLO_CLASS_STAIN;
            next.damage_detected = next.damage_detected || yolo_class_is_fault(result.class_id);
            if (result.score > next.top_score) {
                next.top_score = result.score;
                next.top_class_id = result.class_id;
            }
        }
        if (next.top_class_id >= 0) {
            snprintf(next.top_class, sizeof(next.top_class), "%s",
                     s_detector->class_name(next.top_class_id));
        }

        if (s_status_mtx && xSemaphoreTake(s_status_mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
            next.sequence = s_status.sequence + 1;
            s_status = next;
            xSemaphoreGive(s_status_mtx);
        }
        s_last_completed_us = inference_end_us;

        heap_caps_free(rgb888_buf);
        free(frame.jpeg_data);
        esp_task_wdt_reset();
    }
}

void inference_task_start(void) {
    s_frame_mtx = xSemaphoreCreateMutex();
    s_frame_sem = xSemaphoreCreateBinary();
    s_status_mtx = xSemaphoreCreateMutex();
    if (!s_frame_mtx || !s_frame_sem || !s_status_mtx) {
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

bool inference_status_get(inference_status_t *status) {
    if (!status || !s_status_mtx) return false;
    if (xSemaphoreTake(s_status_mtx, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    *status = s_status;
    xSemaphoreGive(s_status_mtx);
    return true;
}
