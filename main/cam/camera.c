#include "camera.h"
#include "esp_log.h"
#include "esp_camera.h"

static const char *TAG = "CAM_MOD";

esp_err_t camera_module_init() {
    camera_config_t camera_config = {
        .pin_pwdn = -1,
        .pin_reset = -1,
        .pin_xclk = 15,
        .pin_sccb_sda = 4,
        .pin_sccb_scl = 5,
        .pin_d0 = 11,
        .pin_d1 = 9,
        .pin_d2 = 8,
        .pin_d3 = 10,
        .pin_d4 = 12,
        .pin_d5 = 18,
        .pin_d6 = 17,
        .pin_d7 = 16,
        .pin_vsync = 6,
        .pin_href = 7,
        .pin_pclk = 13,
        .xclk_freq_hz = 24000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_320X320,
        .jpeg_quality = 10,
        .fb_count = 3,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };

    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera Init Failed: %s", esp_err_to_name(err));
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        // --- Display orientation ---
        s->set_hmirror(s, 1);
        s->set_vflip(s, 0);

        s->set_aec2(s, 1);          // auto exposure
        s->set_ae_level(s, 0);      // exposure compensation

        s->set_whitebal(s, 1);      // enable AWB
        s->set_awb_gain(s, 1);      // enable AWB gain
/*
        // --- Exposure & Gain ---
        s->set_aec2(s, 1);          // auto exposure
        s->set_ae_level(s, 0);      // exposure compensation
        s->set_gain_ctrl(s, 1);     // auto gain
        s->set_agc_gain(s, 0);      // gain ceiling (0=2x, max)

        // --- White Balance: force auto mode ---
        s->set_whitebal(s, 1);      // enable AWB
        s->set_awb_gain(s, 1);      // enable AWB gain

        // --- Color tuning: reduce green bias ---
        // OV5640 often has green cast from IR leakage or sensor bias.
        // Lower saturation slightly and let AWB compensate.
        s->set_saturation(s, -2);   // slight desaturation to tame green
        s->set_brightness(s, 0);
        s->set_contrast(s, 0);

        // --- Image processing ---
        s->set_special_effect(s, 0); // no effect
        s->set_wpc(s, 1);            // white pixel correction
        s->set_dcw(s, 1);            // downsample enable
        s->set_sharpness(s, 0);

        s->set_wb_mode(s, 1);       // auto white balance mode
*/
        ESP_LOGI(TAG, "Sensor color calibration applied");
    }

    return ESP_OK;
}

camera_fb_t *camera_module_capture() {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "Camera capture failed!");
    }
    return fb;
}

void camera_module_return(camera_fb_t *fb) {
    if (fb) esp_camera_fb_return(fb);
}
