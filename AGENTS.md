# AGENTS.md

This file provides guidance to Codex (Codex.ai/code) when working with code in this repository.

## Project Overview

ESP32-S3 太阳能板智能检测相机 — 边缘 AI 设备，集成 YOLO11n INT8 缺陷检测 + MJPEG 视频流 + 环境传感器。检测 6 类太阳能板缺陷（鸟粪/灰尘/积雪/电损伤/物理损伤/清洁）。

## Build Commands

```bash
# 首次或配置变更后清理重建
idf.py fullclean

# 编译
idf.py build

# 烧录并打开串口监控（替换 COM<X>）
idf.py -p COM<X> flash monitor
# 退出监控: Ctrl+]

# 仅配置（menuconfig 已含 LED 类型/GPIO 等自定义菜单）
idf.py menuconfig
```

**前提**：ESP-IDF v5.4+，需先激活 ESP-IDF 环境（`export.ps1` 或 ESP-IDF CMD）。

**Wi-Fi 配置**：复制 `main/wifi/wifi_config.example.h` → `main/wifi/wifi_config.local.h`，填写 SSID 和密码。该文件已 gitignore。

## Model Training Pipeline

训练和部署在不同环境完成，用 `yolov11/train_and_export.py` 的子命令隔离：

```bash
cd yolov11

# ① 数据集划分（按 Roboflow 原图 ID 分组，防泄漏）
python split_dataset.py          # 输出 splits/{train,val,test}.txt

# ② 云端 GPU 训练（AutoDL 等）
python train_and_export.py train # 100 epochs, AdamW, 320×320

# ③ 本地 CPU 导出 + 量化 + 部署
python train_and_export.py export-onnx --weights <best.pt>
python train_and_export.py quantize          # esp-ppq INT8 → .espdl
python train_and_export.py deploy            # 复制到 main/model/
```

依赖分离：`requirements-cloud.txt`（训练 GPU）、`requirements-local-export.txt`（量化 CPU）。

## Architecture

### Firmware (main/)

**入口**：`main_app.cpp` → `app_main()`，初始化顺序：NVS → WiFi → Camera → LED → I2C → OLED → Sensors → 启动 FreeRTOS 任务。

**核心数据流**：
```
OV5640 (320×320 JPEG, Core 0 main loop)
  ├→ http_stream (/stream:80)         — MJPEG 双缓冲，PSRAM 零拷贝
  ├→ inference_task (Core 1)          — JPEG→RGB888→letterbox→量化→ESP-DL推理
  │    └→ yolo_detect                 — DFL 3-head 解码 + NMS (conf=0.20, iou=0.50)
  │         └→ http_stream (/annotated:8080) — 标注结果 JPEG
  └→ LED 状态指示 (缺陷=红/正常=绿)
```

**FreeRTOS 任务分布**：
- Core 0：主循环（采集+分发帧）、OLED 更新（2s）、HTTP 服务器
- Core 1：DHT11 读取（3s）、YOLO 推理任务（64KB 栈，~4.6s/帧）

**关键模块**：

| 模块 | 职责 |
|------|------|
| `cam/camera.c` | OV5640 初始化，320×320 JPEG，3 帧缓冲 PSRAM，grab-latest |
| `wifi/wifi.c` | STA 模式，自动重连，凭据从 local header 读取 |
| `http_stream/http_stream.c` | 双 HTTP 服务器：80 端口（仪表盘+MJPEG 流+传感器 API）、8080（标注 JPEG） |
| `yolo_detect/yolo_detect.cpp` | ESP-DL 模型加载、letterbox 预处理、INT8 量化、DFL 解码、NMS、位图字体标注渲染 |
| `inference_task/inference_task.cpp` | FreeRTOS 任务封装，接收帧→解码→推理→标注→发布 |
| `sensor_data/sensor_data.c` | mutex 保护的温湿度数据存储，供 HTTP API 和 OLED 共享读取 |
| `yolo_classes.h` | 6 类缺陷枚举 + 脏污权重/告警优先级/故障判定/湿洗阻塞等业务逻辑函数 |
| `dashboard.html` | 嵌入式 Web 仪表盘（Chart.js 图表、双画面、模拟天气、规则引擎） |

**本地组件**：`components/ssd1306/` — 自定义 SSD1306 OLED 驱动（I2C, 128×64, framebuffer）。

### Model Training (yolov11/)

- `train_and_export.py` — 主流水线脚本，5 个子命令（train/validate/export-onnx/quantize/deploy）
- `split_dataset.py` — 按 Roboflow 原图 ID 分组，70/15/15 无数据泄漏划分
- `data.yaml` — 数据集配置，引用 `splits/` 下的清单文件
- 导出时自动重写 Ultralytics 模型头为 ESPDetect/ESPAttention，输出 6 路分离输出供 ESP-DL 消费

## Hardware Pin Map

**两组独立 I2C 总线**：
- 摄像头专用 SCCB：SDA=GPIO4, SCL=GPIO5
- 传感器共享 I2C0：SDA=GPIO2, SCL=GPIO3（SHT20@0x40, TSL2584@0x39, SSD1306@0x3C）

**ADC**：MAX471 → GPIO1 (ADC1_CH0)
**DHT11**：GPIO38（单线 bit-bang，微秒级时序临界区）
**LED**：缺陷=GPIO14(红), 正常=GPIO19(绿), 闪烁=GPIO8(可通过 menuconfig 配置)

## Key Configuration Files

| 文件 | 用途 |
|------|------|
| `sdkconfig` | 完整构建配置（勿手动编辑，用 menuconfig） |
| `sdkconfig.defaults` | 自定义默认值（分区表、60s WDT 看门狗） |
| `partitions.csv` | 16MB Flash 分区：nvs(24KB)+phy(4KB)+factory(14MB) |
| `main/idf_component.yml` | 外部组件依赖声明（esp-dl, esp32-camera, led_strip） |
| `main/yolo_detect/yolo_detect.hpp` | 推理阈值：`YOLO_CONF_THRESHOLD`(0.20)、`YOLO_IOU_THRESHOLD`(0.50) |
| `main/Kconfig.projbuild` | menuconfig 自定义菜单（LED 类型/GPIO/闪烁周期） |

## Conventions

- C/C++ 混合：传感器驱动用 C，YOLO 推理和主入口用 C++
- WiFi 凭据通过 gitignored 的 `wifi_config.local.h` 管理，不提交到仓库
- 帧缓冲全部分配在 PSRAM（8MB），推理输出和 HTTP 缓冲使用双缓冲模式
- 模型文件 `main/model/yolo11n_esp32s3.espdl` 通过 `idf_component.yml` 的 `EMBED_FILES` 嵌入固件
- `dashboard.html` 同样通过 `EMBED_FILES` 嵌入，修改后需重新编译
