#include "http_stream.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <sys/poll.h>
#include <stdlib.h>

static const char *TAG = "HTTP_STREAM";

static esp_err_t root_handler(httpd_req_t *req);
static esp_err_t stream_handler(httpd_req_t *req);
static esp_err_t annotated_handler(httpd_req_t *req);

// Double-buffer for zero-copy streaming: the stream handler
// reads from one side while the main loop writes to the other.
#define MAX_JPEG (64 * 1024)   // 64KB — safe for 320×320 JPEG (~8-10KB)

typedef struct {
    uint8_t *data;
    size_t   len;
    int      serial;
} frame_buf_t;

static SemaphoreHandle_t s_frame_sem = NULL;   // signals new frame ready
static SemaphoreHandle_t s_write_mtx = NULL;    // protects writing side
static frame_buf_t s_frames[3];                 // double buffer
static volatile int s_write_idx = 0;            // index being written to

// Annotated (detection result) image — double buffer + long-poll semaphore
static uint8_t *s_annot_bufs[2] = {NULL, NULL};
static volatile size_t s_annot_lens[2] = {0, 0};
static volatile int s_annot_write_idx = 0;
static SemaphoreHandle_t s_annot_sem = NULL;

static char s_ip_str[16] = "0.0.0.0";

esp_err_t start_stream_server(void) {
    s_frame_sem  = xSemaphoreCreateBinary();
    s_write_mtx  = xSemaphoreCreateMutex();
    s_annot_sem  = xSemaphoreCreateCounting(1, 0);

    s_frames[0].data = (uint8_t *)heap_caps_malloc(MAX_JPEG, MALLOC_CAP_SPIRAM);
    s_frames[1].data = (uint8_t *)heap_caps_malloc(MAX_JPEG, MALLOC_CAP_SPIRAM);
    s_annot_bufs[0]  = (uint8_t *)heap_caps_malloc(MAX_JPEG, MALLOC_CAP_SPIRAM);
    s_annot_bufs[1]  = (uint8_t *)heap_caps_malloc(MAX_JPEG, MALLOC_CAP_SPIRAM);

    if (!s_frame_sem || !s_write_mtx || !s_annot_sem ||
        !s_frames[0].data || !s_frames[1].data ||
        !s_annot_bufs[0] || !s_annot_bufs[1]) {
        ESP_LOGE(TAG, "Failed to allocate stream resources");
        return ESP_FAIL;
    }
    s_frames[0].len = s_frames[1].len = 0;
    s_frames[0].serial = s_frames[1].serial = -1;

    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&ip_info.ip));
    }

    // --- Main stream: port 80, MJPEG ---
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 6144;
    config.max_uri_handlers = 8;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ESP_FAIL;
    }

    const httpd_uri_t stream_uri = {
        .uri = "/stream", .method = HTTP_GET,
        .handler = stream_handler, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &stream_uri);

    const httpd_uri_t root_uri = {
        .uri = "/", .method = HTTP_GET,
        .handler = root_handler, .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &root_uri);

    // --- Annotated image: port 8080 ---
    httpd_config_t annot_cfg = HTTPD_DEFAULT_CONFIG();
    annot_cfg.server_port    = 8080;
    annot_cfg.ctrl_port      = 0;
    annot_cfg.stack_size     = 4096;
    annot_cfg.max_uri_handlers = 4;

    httpd_handle_t annot_srv = NULL;
    if (httpd_start(&annot_srv, &annot_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start annot server on port 8080");
        return ESP_FAIL;
    }

    const httpd_uri_t annot_uri = {
        .uri = "/annotated", .method = HTTP_GET,
        .handler = annotated_handler, .user_ctx = NULL
    };
    httpd_register_uri_handler(annot_srv, &annot_uri);

    ESP_LOGI(TAG, "Stream:  http://" IPSTR "/stream",  IP2STR(&ip_info.ip));
    ESP_LOGI(TAG, "Annot:   http://" IPSTR ":8080/annotated", IP2STR(&ip_info.ip));
    return ESP_OK;
}

// --- Called from main loop: write to the inactive buffer slot ---
void stream_update_frame(const uint8_t *jpeg_data, size_t jpeg_len) {
    if (!s_write_mtx || !s_frames[0].data) return;
    if (jpeg_len > MAX_JPEG) jpeg_len = MAX_JPEG;

    // Quick try-lock so we never stall the capture loop
    if (xSemaphoreTake(s_write_mtx, 0) != pdTRUE) return;

    int idx = s_write_idx;
    memcpy(s_frames[idx].data, jpeg_data, jpeg_len);
    s_frames[idx].len    = jpeg_len;
    s_frames[idx].serial++;

    // Swap active buffer — the old active buffer is now read-only
    s_write_idx ^= 1;

    xSemaphoreGive(s_write_mtx);

    // Wake the stream handler
    xSemaphoreGive(s_frame_sem);
}

void stream_update_annotated(const uint8_t *jpeg_data, size_t jpeg_len) {
    if (!s_annot_bufs[0] || !s_annot_bufs[1] || !s_annot_sem) return;
    if (jpeg_len > MAX_JPEG) jpeg_len = MAX_JPEG;

    int w = s_annot_write_idx;
    memcpy(s_annot_bufs[w], jpeg_data, jpeg_len);
    s_annot_lens[w] = jpeg_len;
    s_annot_write_idx ^= 1;
    xSemaphoreGive(s_annot_sem);
}

static bool client_is_connected(httpd_req_t *req) {
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) return false;
    struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
    return poll(&pfd, 1, 0) >= 0 && !(pfd.revents & (POLLERR | POLLHUP | POLLNVAL));
}

// --- MJPEG stream handler: event-driven, zero-copy from double buffer ---
static esp_err_t stream_handler(httpd_req_t *req) {
    char part_buf[128];
    int  last_serial = -1;

    httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");

    while (client_is_connected(req)) {
        // Block until a new frame is posted (zero CPU when idle)
        if (xSemaphoreTake(s_frame_sem, pdMS_TO_TICKS(1000)) != pdTRUE)
            continue;

        // Read the most recent completed buffer (the *other* index)
        int read_idx = s_write_idx ^ 1;
        size_t len   = s_frames[read_idx].len;
        int    serial = s_frames[read_idx].serial;

        if (len == 0 || serial == last_serial) continue;
        last_serial = serial;

        // Send directly from PSRAM — no local copy needed
        int hlen = snprintf(part_buf, sizeof(part_buf),
            "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
            (unsigned)len);

        if (httpd_resp_send_chunk(req, part_buf, hlen) != ESP_OK) break;
        if (httpd_resp_send_chunk(req, (const char *)s_frames[read_idx].data, len) != ESP_OK) break;
        if (httpd_resp_send_chunk(req, "\r\n", 2) != ESP_OK) break;
    }

    ESP_LOGI(TAG, "Stream client disconnected");
    return ESP_OK;
}

// --- Annotated JPEG snapshot (serve latest frame, no blocking) ---
static esp_err_t annotated_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache,no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    if (!client_is_connected(req)) {
        return ESP_FAIL;
    }

    int r = s_annot_write_idx ^ 1;
    size_t len = s_annot_lens[r];
    if (len == 0) {
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    return httpd_resp_send(req, (const char *)s_annot_bufs[r], len);
}

// --- Root HTML: embedded dashboard from binary ---
extern const uint8_t dashboard_html_start[] asm("_binary_dashboard_html_start");
extern const uint8_t dashboard_html_end[]   asm("_binary_dashboard_html_end");

static esp_err_t root_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    size_t len = dashboard_html_end - dashboard_html_start;
    httpd_resp_send(req, (const char *)dashboard_html_start, len);
    return ESP_OK;
}
