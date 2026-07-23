idf_build_get_property(component_targets __COMPONENT_TARGETS)
if ("___idf_espressif__esp-dl" IN_LIST component_targets)
    idf_component_get_property(espdl_dir espressif__esp-dl COMPONENT_DIR)
elseif("___idf_esp-dl" IN_LIST component_targets)
    idf_component_get_property(espdl_dir esp-dl COMPONENT_DIR)
endif()
set(cmake_dir ${espdl_dir}/fbs_loader/cmake)
include(${cmake_dir}/utilities.cmake)

idf_component_get_property(cam_dir espressif__esp32-camera COMPONENT_DIR)

set(embed_files model/yolo11n_esp32s3.espdl)

idf_component_register(
    SRCS "main_app.cpp"
         "I2C.c"
         "cam/camera.c"
         "wifi/wifi.c"
         "http_stream/http_stream.c"
         "blink/blink.c"
         "yolo_detect/yolo_detect.cpp"
         "inference_task/inference_task.cpp"
         "oled/oled.c"
         "led/led.c"
         "max471/MAX471.c"
         "sht20/SHT20.c"
         "tsl2584/TSL2584.c"
         "dht11/dht11.c"
         "sensor_data/sensor_data.c"
    INCLUDE_DIRS "." "cam" "wifi" "http_stream" "blink"
                 "yolo_detect" "inference_task"
                 "oled" "led"
                 "max471" "sht20" "tsl2584"
                 "dht11" "sensor_data"
                 "${cam_dir}/target/jpeg_include"
    PRIV_REQUIRES esp_driver_gpio esp_adc esp_driver_i2c esp_http_server esp_netif esp_timer nvs_flash esp_wifi ssd1306
)

target_add_aligned_binary_data(${COMPONENT_LIB} ${embed_files} BINARY)
target_add_binary_data(${COMPONENT_LIB} dashboard.html BINARY)
