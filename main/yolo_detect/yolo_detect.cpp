#include "yolo_detect.hpp"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "dl_model_base.hpp"
#include "dl_tensor_base.hpp"
#include "img_converters.h"
#include <cstring>
#include <cmath>
#include <algorithm>
#include <vector>

static const char *TAG = "YOLO_DET";

static const char *CLASS_NAMES[YOLO_NUM_CLASSES] = {
    "bird_drop", "clean", "dust", "electrical_damage",
    "physical_damage", "snow_covered"
};

extern const uint8_t model_espdl_start[] asm("_binary_yolo11n_esp32s3_espdl_start");

struct GridStride {
    int grid_h, grid_w;
    int stride;
};

static const GridStride HEADS[3] = {
    {40, 40, 8},
    {20, 20, 16},
    {10, 10, 32},
};

static inline float sigmoid(float x) {
    return 1.0f / (1.0f + std::exp(-x));
}

static inline float box_iou(const detection_t &a, const detection_t &b) {
    int ix1 = std::max(a.x1, b.x1);
    int iy1 = std::max(a.y1, b.y1);
    int ix2 = std::min(a.x2, b.x2);
    int iy2 = std::min(a.y2, b.y2);
    int iw = std::max(0, ix2 - ix1);
    int ih = std::max(0, iy2 - iy1);
    float inter = (float)(iw * ih);
    float area_a = (float)((a.x2 - a.x1) * (a.y2 - a.y1));
    float area_b = (float)((b.x2 - b.x1) * (b.y2 - b.y1));
    return inter / (area_a + area_b - inter + 1e-6f);
}

static void nms(std::vector<detection_t> &dets, float iou_thresh) {
    if (dets.empty()) return;
    std::sort(dets.begin(), dets.end(),
              [](const detection_t &a, const detection_t &b) { return a.score > b.score; });
    std::vector<bool> keep(dets.size(), true);
    for (size_t i = 0; i < dets.size(); i++) {
        if (!keep[i]) continue;
        for (size_t j = i + 1; j < dets.size(); j++) {
            if (!keep[j]) continue;
            if (box_iou(dets[i], dets[j]) > iou_thresh) {
                keep[j] = false;
            }
        }
    }
    size_t out = 0;
    for (size_t i = 0; i < dets.size(); i++) {
        if (keep[i]) dets[out++] = dets[i];
    }
    dets.resize(out);
}

static void decode_head(
    const int8_t *box_data, const int8_t *score_data,
    int grid_h, int grid_w, int stride,
    float inv_scale_x, float inv_scale_y,
    int pad_w, int pad_h,
    float score_scale, float box_scale,
    std::vector<detection_t> &dets, float conf_thresh)
{
    constexpr int DFL_CHANNELS = 16;
    float logit_thresh = std::log(conf_thresh / (1.0f - conf_thresh + 1e-9f));
    int8_t score_thr = (int8_t)std::ceil(logit_thresh / score_scale);

    for (int y = 0; y < grid_h; y++) {
        for (int x = 0; x < grid_w; x++) {
            int score_offset = (y * grid_w + x) * YOLO_NUM_CLASSES;
            int8_t best_score_q = score_thr;
            int best_class = 0;
            for (int c = 0; c < YOLO_NUM_CLASSES; c++) {
                int8_t sq = score_data[score_offset + c];
                if (sq > best_score_q) {
                    best_score_q = sq;
                    best_class = c;
                }
            }
            if (best_score_q <= score_thr) continue;

            int box_offset = (y * grid_w + x) * 64;
            float dist[4] = {0};
            for (int side = 0; side < 4; side++) {
                int off = box_offset + side * DFL_CHANNELS;
                float max_logit = -1e9f;
                for (int bin = 0; bin < DFL_CHANNELS; bin++) {
                    float logit = (float)box_data[off + bin] * box_scale;
                    if (logit > max_logit) max_logit = logit;
                }
                float softmax_sum = 0;
                float weighted_sum = 0;
                for (int bin = 0; bin < DFL_CHANNELS; bin++) {
                    float val = std::exp((float)box_data[off + bin] * box_scale - max_logit);
                    softmax_sum += val;
                    weighted_sum += val * bin;
                }
                dist[side] = weighted_sum / (softmax_sum + 1e-9f);
            }

            float cx = (x + 0.5f) * stride;
            float cy = (y + 0.5f) * stride;
            float x1 = (cx - dist[0] * stride - pad_w) * inv_scale_x;
            float y1 = (cy - dist[1] * stride - pad_h) * inv_scale_y;
            float x2 = (cx + dist[2] * stride - pad_w) * inv_scale_x;
            float y2 = (cy + dist[3] * stride - pad_h) * inv_scale_y;

            float logit = (float)best_score_q * score_scale;
            if (logit < -10.0f) logit = -10.0f;
            if (logit > 10.0f) logit = 10.0f;
            float score_f = sigmoid(logit);
            detection_t det;
            det.class_id = best_class;
            det.score = score_f;
            det.x1 = (int)(x1 + 0.5f); if (det.x1 < 0) det.x1 = 0;
            det.y1 = (int)(y1 + 0.5f); if (det.y1 < 0) det.y1 = 0;
            det.x2 = (int)(x2 + 0.5f);
            det.y2 = (int)(y2 + 0.5f);
            dets.push_back(det);

            // DFL diagnostic: first detection in each head (per frame)
            if (dets.size() == 1 && score_f > 0.01f) {
                ESP_LOGI(TAG, "DFL: s=%d cell(%d,%d) cx=%.1f cy=%.1f "
                         "l=%.2f t=%.2f r=%.2f b=%.2f cls=%s score=%.3f",
                         stride, x, y, cx, cy,
                         dist[0], dist[1], dist[2], dist[3],
                         CLASS_NAMES[best_class], score_f);
            }
        }
    }
}

YOLODetector::YOLODetector() {}
YOLODetector::~YOLODetector() {
    if (model_) {
        delete (dl::Model *)model_;
    }
}

esp_err_t YOLODetector::init() {
    ESP_LOGI(TAG, "Loading ESP-DL model from flash rodata (param_copy=false)...");

    model_ = (void *)new dl::Model(
        (const char *)model_espdl_start,
        fbs::MODEL_LOCATION_IN_FLASH_RODATA,
        0,
        dl::MEMORY_MANAGER_GREEDY,
        nullptr,
        true
    );
    if (!model_) {
        ESP_LOGE(TAG, "Failed to create model");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Running model self-test...");
    esp_err_t ret = ((dl::Model *)model_)->test();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Model self-test PASSED!");
    } else {
        ESP_LOGE(TAG, "Model self-test FAILED! (ret=0x%x)", ret);
    }

    ((dl::Model *)model_)->profile_memory();

    auto inputs = ((dl::Model *)model_)->get_inputs();
    for (auto &kv : inputs) {
        ESP_LOGI(TAG, "Model input: name=%s shape=%s exp=%d",
                 kv.first.c_str(), dl::vector_to_string(kv.second->get_shape()).c_str(),
                 (int)kv.second->exponent);
    }
    auto model_outputs = ((dl::Model *)model_)->get_outputs();
    for (auto &kv : model_outputs) {
        ESP_LOGI(TAG, "Model output: name=%s shape=%s exp=%d",
                 kv.first.c_str(), dl::vector_to_string(kv.second->get_shape()).c_str(),
                 (int)kv.second->exponent);
    }

    ESP_LOGI(TAG, "Profiling per-module latency (sorted by time)...");
    ((dl::Model *)model_)->profile_module(true);

    return ESP_OK;
}

std::vector<detection_t> YOLODetector::detect(
    const uint8_t *rgb888_data,
    int img_width, int img_height)
{
    std::vector<detection_t> results;
    if (!model_) {
        ESP_LOGE(TAG, "Model not initialized");
        return results;
    }
    dl::Model *model = (dl::Model *)model_;

    int64_t t0 = esp_timer_get_time();

    dl::TensorBase *model_input = model->get_inputs().begin()->second;
    int input_exponent = model_input->exponent;
    int8_t *tensor = (int8_t *)model_input->data;

    int dst_w = YOLO_IMGSZ, dst_h = YOLO_IMGSZ;
    float scale = std::min((float)dst_w / img_width, (float)dst_h / img_height);
    int new_w = (int)(img_width * scale);
    int new_h = (int)(img_height * scale);
    int pad_x = (dst_w - new_w) / 2;
    int pad_y = (dst_h - new_h) / 2;

    int8_t pad_val = (int8_t)((114 * 128 + 127) / 255);
    for (int i = 0; i < dst_w * dst_h * 3; i++) {
        tensor[i] = pad_val;
    }

    for (int dy = 0; dy < new_h; dy++) {
        int sy = (int)(dy / scale);
        if (sy >= img_height) sy = img_height - 1;
        for (int dx = 0; dx < new_w; dx++) {
            int sx = (int)(dx / scale);
            if (sx >= img_width) sx = img_width - 1;

            int src_idx = (sy * img_width + sx) * 3;
            int r = rgb888_data[src_idx];
            int g = rgb888_data[src_idx + 1];
            int b = rgb888_data[src_idx + 2];

            int dst_idx = ((pad_y + dy) * dst_w + (pad_x + dx)) * 3;
            int qr = (r * 128 + 127) / 255;
            int qg = (g * 128 + 127) / 255;
            int qb = (b * 128 + 127) / 255;
            tensor[dst_idx]     = (int8_t)(qr > 127 ? 127 : qr);
            tensor[dst_idx + 1] = (int8_t)(qg > 127 ? 127 : qg);
            tensor[dst_idx + 2] = (int8_t)(qb > 127 ? 127 : qb);
        }
    }

    int8_t minv = 127, maxv = -128;
    int32_t sum = 0;
    int total = dst_w * dst_h * 3;
    for (int i = 0; i < total; i++) {
        if (tensor[i] < minv) minv = tensor[i];
        if (tensor[i] > maxv) maxv = tensor[i];
        sum += tensor[i];
    }
    ESP_LOGI(TAG, "Input exponent=%d, quant_scale=2^%d=%.4f, dequant_scale=2^%d=%.6f",
             input_exponent, -input_exponent, std::ldexp(1.0f, -input_exponent),
             input_exponent, std::ldexp(1.0f, input_exponent));
    ESP_LOGI(TAG, "Input int8 range: %d ~ %d, mean=%.2f",
             minv, maxv, (float)sum / total);

    int64_t t1 = esp_timer_get_time();

    model->run(dl::RUNTIME_MODE_MULTI_CORE);

    int64_t t2 = esp_timer_get_time();

    auto outputs = model->get_outputs();
    float inv_scale_x = (float)img_width / (float)(dst_w - 2 * pad_x);
    float inv_scale_y = (float)img_height / (float)(dst_h - 2 * pad_y);

    float max_scores[3] = {0};
    int best_classes[3] = {-1, -1, -1};

    for (int h = 0; h < 3; h++) {
        char box_name[16], score_name[16];
        snprintf(box_name, sizeof(box_name), "box%d", h);
        snprintf(score_name, sizeof(score_name), "score%d", h);

        auto box_it = outputs.find(box_name);
        auto score_it = outputs.find(score_name);
        if (box_it == outputs.end() || score_it == outputs.end()) {
            ESP_LOGW(TAG, "Output %s/%s not found", box_name, score_name);
            continue;
        }

        const int8_t *box_ptr = (const int8_t *)box_it->second->data;
        const int8_t *score_ptr = (const int8_t *)score_it->second->data;

        auto &box_tensor = box_it->second;
        auto &score_tensor = score_it->second;

        int score_elem = score_tensor->get_size();
        int8_t smin = 127, smax = -128;
        int32_t ssum = 0;
        int hist_bins[8] = {0};
        for (int i = 0; i < score_elem; i++) {
            int8_t v = score_ptr[i];
            if (v < smin) smin = v;
            if (v > smax) smax = v;
            ssum += v;
            int bin = (v + 128) * 8 / 256;
            if (bin < 0) bin = 0;
            if (bin > 7) bin = 7;
            hist_bins[bin]++;
        }
        ESP_LOGI(TAG, "Head%d score int8: min=%d max=%d mean=%.1f elems=%d",
                 h, smin, smax, (float)ssum / score_elem, score_elem);

        int8_t bmin = 127, bmax = -128;
        int32_t bsum = 0;
        int box_elem = box_tensor->get_size();
        for (int i = 0; i < box_elem; i++) {
            if (box_ptr[i] < bmin) bmin = box_ptr[i];
            if (box_ptr[i] > bmax) bmax = box_ptr[i];
            bsum += box_ptr[i];
        }
        ESP_LOGI(TAG, "Head%d box int8: min=%d max=%d mean=%.1f elems=%d",
                 h, bmin, bmax, (float)bsum / box_elem, box_elem);

        float score_scale_dbg = std::ldexp(1.0f, (score_it->second->exponent));
        int grid_h = (h == 0) ? 40 : (h == 1) ? 20 : 10;
        int grid_w = grid_h;
        int above_zero = 0, total_scores = grid_h * grid_w * YOLO_NUM_CLASSES;
        for (int i = 0; i < total_scores; i++) {
            float s = sigmoid((float)score_ptr[i] * score_scale_dbg);
            if (s > max_scores[h]) {
                max_scores[h] = s;
                best_classes[h] = i % YOLO_NUM_CLASSES;
            }
            if (score_ptr[i] >= 0) above_zero++;
        }
        ESP_LOGI(TAG, "Head%d: max=%.4f cls=%d  scores≥0: %d/%d",
                 h, max_scores[h], best_classes[h], above_zero, total_scores);
    }

    for (int h = 0; h < 3; h++) {
        char box_name[16], score_name[16];
        snprintf(box_name, sizeof(box_name), "box%d", h);
        snprintf(score_name, sizeof(score_name), "score%d", h);

        auto box_it = outputs.find(box_name);
        auto score_it = outputs.find(score_name);
        if (box_it == outputs.end() || score_it == outputs.end()) continue;

        const int8_t *box_ptr = (const int8_t *)box_it->second->data;
        const int8_t *score_ptr = (const int8_t *)score_it->second->data;

        int score_exp = score_it->second->exponent;
        int box_exp   = box_it->second->exponent;
        float score_scale = std::ldexp(1.0f, score_exp);
        float box_scale   = std::ldexp(1.0f, box_exp);

        decode_head(
            box_ptr, score_ptr,
            HEADS[h].grid_h, HEADS[h].grid_w, HEADS[h].stride,
            inv_scale_x, inv_scale_y,
            pad_x, pad_y,
            score_scale, box_scale,
            results, YOLO_CONF_THRESHOLD
        );
    }

    nms(results, YOLO_IOU_THRESHOLD);
    if (results.size() > YOLO_MAX_DETECTIONS) {
        results.resize(YOLO_MAX_DETECTIONS);
    }

    int64_t t3 = esp_timer_get_time();

    ESP_LOGI(TAG, "=== Time Breakdown ===");
    ESP_LOGI(TAG, "  Letterbox + quantize:               %lld ms", (long long)((t1 - t0) / 1000));
    ESP_LOGI(TAG, "  Model inference:                    %lld ms", (long long)((t2 - t1) / 1000));
    ESP_LOGI(TAG, "  Post-process (decode + NMS):        %lld ms", (long long)((t3 - t2) / 1000));
    ESP_LOGI(TAG, "  Total:                               %lld ms", (long long)((t3 - t0) / 1000));

    return results;
}

void YOLODetector::print_results(const std::vector<detection_t> &results) {
    ESP_LOGI(TAG, "--- Detection Results (%zu objects) ---", results.size());
    for (size_t i = 0; i < results.size(); i++) {
        ESP_LOGI(TAG, "[%zu] %s: %.3f [%d,%d,%d,%d]",
                 i,
                 class_name(results[i].class_id),
                 results[i].score,
                 results[i].x1, results[i].y1,
                 results[i].x2, results[i].y2);
    }
}

const char* YOLODetector::class_name(int class_id) const {
    if (class_id >= 0 && class_id < YOLO_NUM_CLASSES) {
        return CLASS_NAMES[class_id];
    }
    return "Unknown";
}

static const uint32_t CLASS_COLORS[YOLO_NUM_CLASSES] = {
    0x00CC44, // bird_drop      - orange (high visibility)
    0x22CC22, // clean          - green
    0xCCCC00, // dust           - yellow
    0xCC2200, // electrical     - red
    0x8888FF, // physical       - blue
    0x00CCCC, // snow_covered   - cyan
};

static inline int clamp_i(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static void set_px(uint8_t *buf, int w, int h, int x, int y,
                  uint8_t r, uint8_t g, uint8_t b) {
    if (x >= 0 && x < w && y >= 0 && y < h) {
        int idx = (y * w + x) * 3;
        buf[idx] = r; buf[idx+1] = g; buf[idx+2] = b;
    }
}

static void draw_rect(uint8_t *buf, int w, int h,
                      int x1, int y1, int x2, int y2,
                      uint8_t r, uint8_t g, uint8_t b, int t) {
    x1 = clamp_i(x1, 0, w-1); y1 = clamp_i(y1, 0, h-1);
    x2 = clamp_i(x2, 0, w-1); y2 = clamp_i(y2, 0, h-1);
    for (int i = 0; i < t; i++) {
        for (int x = x1; x <= x2; x++) { set_px(buf,w,h,x,y1,r,g,b); set_px(buf,w,h,x,y2,r,g,b); }
        for (int y = y1; y <= y2; y++) { set_px(buf,w,h,x1,y,r,g,b); set_px(buf,w,h,x2,y,r,g,b); }
    }
}

static void draw_fill(uint8_t *buf, int w, int h,
                       int x1, int y1, int x2, int y2,
                       uint8_t r, uint8_t g, uint8_t b) {
    x1 = clamp_i(x1, 0, w-1); y1 = clamp_i(y1, 0, h-1);
    x2 = clamp_i(x2, 0, w-1); y2 = clamp_i(y2, 0, h-1);
    for (int y = y1; y <= y2; y++) {
        int idx = (y * w + x1) * 3;
        for (int x = x1; x <= x2; x++, idx += 3) {
            buf[idx] = r; buf[idx+1] = g; buf[idx+2] = b;
        }
    }
}


// 5x7 bitmap font (0-9, A-Z, a-z, .: %_-)
static const uint8_t FONT5X7[95][7] = {
    {0x00,0x00,0x00,0x00,0x00}, // space
    {0x08,0x08,0x08,0x00,0x08}, // !
    {0x14,0x14,0x14,0x00,0x00}, // "
    {0x14,0x3E,0x14,0x3E,0x14}, // #
    {0x04,0x1A,0x3F,0x12,0x02}, // $
    {0x1E,0x29,0x26,0x28,0x16}, // %
    {0x10,0x28,0x24,0x14,0x0C}, // &
    {0x08,0x08,0x08,0x00,0x08}, // '
    {0x06,0x09,0x11,0x21,0x12}, // (
    {0x12,0x21,0x11,0x09,0x06}, // )
    {0x0C,0x14,0x3E,0x14,0x0C}, // *
    {0x08,0x08,0x3E,0x08,0x08}, // +
    {0x00,0x20,0x10,0x20,0x00}, // ,
    {0x08,0x08,0x08,0x08,0x08}, // -
    {0x00,0x00,0x20,0x00,0x00}, // .
    {0x01,0x02,0x04,0x08,0x10}, // /
    {0x1E,0x33,0x31,0x33,0x1E}, // 0
    {0x04,0x06,0x04,0x06,0x04}, // 1
    {0x0E,0x11,0x11,0x11,0x0E}, // 2
    {0x1F,0x21,0x01,0x01,0x1E}, // 3
    {0x13,0x15,0x19,0x11,0x13}, // 4
    {0x1F,0x11,0x0D,0x03,0x07}, // 5
    {0x1E,0x21,0x15,0x09,0x16}, // 6
    {0x07,0x09,0x11,0x11,0x0F}, // 7
    {0x1F,0x11,0x1F,0x11,0x1F}, // 8
    {0x1F,0x11,0x0F,0x01,0x1E}, // 9
    {0x00,0x0C,0x0C,0x00,0x00}, // :
    {0x00,0x0C,0x0C,0x0C,0x0C}, // ;
    {0x04,0x08,0x10,0x08,0x04}, // <
    {0x18,0x18,0x18,0x18,0x18}, // =
    {0x0C,0x02,0x04,0x08,0x10}, // >
    {0x1C,0x22,0x10,0x00,0x00}, // ?
    {0x1E,0x31,0x29,0x25,0x1E}, // @
    {0x1F,0x29,0x29,0x29,0x1F}, // A
    {0x1E,0x31,0x31,0x1E,0x31}, // B
    {0x0E,0x11,0x11,0x11,0x0E}, // C
    {0x1E,0x21,0x21,0x21,0x1E}, // D
    {0x1F,0x29,0x25,0x21,0x1F}, // E
    {0x1F,0x29,0x25,0x01,0x01}, // F
    {0x0E,0x15,0x25,0x25,0x1B}, // G
    {0x1F,0x05,0x05,0x05,0x1F}, // H
    {0x00,0x11,0x3F,0x11,0x00}, // I
    {0x08,0x14,0x14,0x22,0x41}, // J
    {0x1F,0x04,0x0C,0x12,0x1F}, // K
    {0x1F,0x01,0x01,0x01,0x1E}, // L
    {0x3F,0x21,0x21,0x21,0x11}, // M
    {0x3F,0x21,0x11,0x21,0x3F}, // N
    {0x1E,0x21,0x21,0x21,0x1E}, // O
    {0x1E,0x31,0x29,0x25,0x1E}, // P
    {0x1E,0x29,0x25,0x21,0x12}, // Q
    {0x1E,0x29,0x25,0x09,0x16}, // R
    {0x0E,0x15,0x15,0x15,0x0E}, // S
    {0x1F,0x01,0x01,0x01,0x0E}, // T
    {0x1E,0x21,0x21,0x21,0x1E}, // U
    {0x03,0x04,0x18,0x04,0x03}, // V
    {0x1F,0x05,0x0C,0x05,0x1F}, // W
    {0x1F,0x05,0x0C,0x12,0x1F}, // X
    {0x1F,0x05,0x0C,0x05,0x0E}, // Y
    {0x1E,0x21,0x01,0x01,0x1E}, // Z
};

static void draw_char_scaled(uint8_t *buf, int w, int h, char c,
                             int cx, int cy, uint8_t r, uint8_t g, uint8_t b, int scale) {
    int idx = (c >= '0' && c <= '9') ? (c - '0' + 16)
            : (c >= 'A' && c <= 'Z') ? (c - 'A' + 33)
            : (c >= 'a' && c <= 'z') ? (c - 'a' + 59)
            : (c == '.') ? 14 : (c == ':') ? 26 : (c == '%') ? 5 : -1;
    if (idx < 0 || idx >= 95) return;
    for (int col = 0; col < 5; col++) {
        uint8_t bits = FONT5X7[idx][col];
        for (int row = 0; row < 7; row++) {
            if (bits & (1 << row)) {
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        set_px(buf, w, h, cx + col * scale + sx, cy + row * scale + sy, r, g, b);
                    }
                }
            }
        }
    }
}

static void draw_text_scaled(uint8_t *buf, int w, int h, const char *str,
                              int x, int y, uint8_t r, uint8_t g, uint8_t b, int scale) {
    for (int i = 0; str[i]; i++)
        draw_char_scaled(buf, w, h, str[i], x + i * 6 * scale, y, r, g, b, scale);
}

uint8_t* YOLODetector::annotate_jpeg(
    const uint8_t *rgb888_data,
    int img_width, int img_height,
    const std::vector<detection_t> &results,
    size_t *out_len)
{
    if (results.empty() || img_width <= 0 || img_height <= 0) {
        *out_len = 0;
        return NULL;
    }

    size_t rgb888_size = img_width * img_height * 3;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(rgb888_size, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "annotate: failed to alloc RGB888 buffer");
        *out_len = 0;
        return NULL;
    }
    memcpy(buf, rgb888_data, rgb888_size);

    int scale = (img_width >= 240) ? 2 : 1;
    int line_thickness = (scale > 1) ? 3 : 2;

    for (size_t i = 0; i < results.size(); i++) {
        const detection_t &d = results[i];
        int color_idx = d.class_id % YOLO_NUM_CLASSES;
        uint32_t color = CLASS_COLORS[color_idx];
        uint8_t cr = (color >> 16) & 0xFF;
        uint8_t cg = (color >> 8) & 0xFF;
        uint8_t cb = color & 0xFF;

        int x1 = d.x1, y1 = d.y1, x2 = d.x2, y2 = d.y2;

        draw_rect(buf, img_width, img_height, x1, y1, x2, y2, cr, cg, cb, line_thickness);

        // label text: "cls 0.xx"
        char label[32];
        snprintf(label, sizeof(label), "%s %.2f", CLASS_NAMES[d.class_id], d.score);
        int font_w = strlen(label) * 6 * scale;
        int font_h = 7 * scale + 4;

        // smart placement: prefer above box, fallback below
        int lx1 = x1, ly1 = y1 - font_h - 2;
        if (ly1 < 0) ly1 = y2 + 2;

        draw_fill(buf, img_width, img_height,
                   lx1, ly1, lx1 + font_w, ly1 + font_h,
                   cr, cg, cb);

        draw_text_scaled(buf, img_width, img_height, label,
                         lx1 + 2 * scale, ly1 + 2, 255, 255, 255, scale); // white text on colored bg
    }

    uint8_t *jpg_buf = NULL;
    size_t jpg_len = 0;
    bool enc_ok = fmt2jpg(buf, rgb888_size, img_width, img_height,
                          PIXFORMAT_RGB888, 15, &jpg_buf, &jpg_len);
    heap_caps_free(buf);

    if (!enc_ok || !jpg_buf) {
        ESP_LOGE(TAG, "annotate: JPEG encode failed");
        *out_len = 0;
        return NULL;
    }

    *out_len = jpg_len;
    return jpg_buf;
}
