# ESP32-S3 太阳能板智能检测相机

基于 ESP32-S3 的边缘 AI 设备，集成 **YOLO11n 缺陷检测** + **MJPEG 实时视频流** + 环境监测。

## 产品说明书

- **产品名称**：ESP32-S3 太阳能板智能检测相机
- **产品类型**：光伏运维边缘 AI 监测终端
- **目标场景**：光伏电站巡检、分布式屋顶监测、无人机/无人车巡检、运维远程检测
- **核心能力**：
  - 太阳能板表面 6 类缺陷实时识别
  - Web 可视化 MJPEG 视频流与检测结果展示
  - SHT20 温湿度、TSL2584 光照、MAX471 电压三参数环境监测
  - SSD1306 OLED 本地状态显示
  - 本地离线 ESP-DL INT8 推理，无需云端依赖

## 项目概述

本项目实现了一个完整的太阳能板缺陷检测系统：

- **实时检测**：6 类缺陷自动识别（鸟粪、灰尘、积雪、电损伤、物理损伤、清洁）
- **视频流**：双路 HTTP 流（原始画面 + 检测标注）
- **环境监测**：温湿度、光照强度、系统电压
- **本地显示**：OLED 实时状态

---

## 硬件配置

### 核心硬件

| 组件 | 型号 | 规格 |
|------|------|------|
| MCU | ESP32-S3-WROOM-1 | 240MHz 双核, 8MB PSRAM, 16MB Flash |
| 摄像头 | OV5640 | 320×320 JPEG, 24MHz XCLK |
| 显示 | SSD1306 OLED | 128×64 I2C |
| 温湿度 | SHT20 | I2C (0x40) |
| 光照 | TSL2584 | I2C (0x39) |
| 相对光照 | 光敏电阻/LM393 四针模块 | ADC1_CH1 |
| 电压检测 | MAX471 | ADC1_CH0 |

### GPIO 引脚分配

#### 摄像头 (OV5640)

| 信号 | GPIO | 说明 |
|------|------|------|
| XCLK | 15 | 24MHz 主时钟输出 |
| SCCB_SDA | 4 | I2C 数据 (摄像头专用) |
| SCCB_SCL | 5 | I2C 时钟 (摄像头专用) |
| D0 | 11 | 数据位 0 |
| D1 | 9 | 数据位 1 |
| D2 | 8 | 数据位 2 |
| D3 | 10 | 数据位 3 |
| D4 | 12 | 数据位 4 |
| D5 | 18 | 数据位 5 |
| D6 | 17 | 数据位 6 |
| D7 | 16 | 数据位 7 |
| VSYNC | 6 | 垂直同步 |
| HREF | 7 | 水平参考 |
| PCLK | 13 | 像素时钟 |
| PWDN | - | 未使用 |
| RESET | - | 未使用 |

#### I2C 传感器总线 (I2C0)

| 信号 | GPIO | 说明 |
|------|------|------|
| SDA | 21 | I2C 数据 |
| SCL | 3 | I2C 时钟 |

**总线设备**：
- SHT20 (温湿度): 0x40
- TSL2584 (光照): 0x39
- SSD1306 (OLED): 0x3C 或 0x3D

#### ADC 电压检测

| 通道 | GPIO | 说明 |
|------|------|------|
| ADC1_CH0 | 1 | MAX471 电压输出 |
| ADC1_CH1 | 2 | 光敏模块 AO，0-100% 相对光照强度 |

光敏模块使用 3.3V 供电：`VCC -> 3V3`、`GND -> GND`、`AO -> GPIO2`，`DO` 暂不连接。该模块未做 lux 标定，输出为原始 ADC、电压和相对光照百分比。

#### LED 状态指示

通过 `idf.py menuconfig` → `APP Configuration` 配置，默认 GPIO 8。

---

## 检测类别

| 类别 | 英文 | 说明 |
|------|------|------|
| 鸟粪 | bird_drop | 鸟类排泄物污染 |
| 清洁 | clean | 太阳能板表面清洁 |
| 灰尘 | dust | 灰尘堆积 |
| 电损伤 | electrical_damage | 电路/接线损坏 |
| 物理损伤 | physical_damage | 裂纹、破损 |
| 积雪 | snow_covered | 雪覆盖 |

---

## 模型信息

| 项目 | 值 |
|------|-----|
| 架构 | YOLO11n (2.6M params, 6.3 GFLOPs) |
| 输入尺寸 | 320×320 RGB |
| 量化 | INT8 (ESP-PPQ) |
| 模型大小 | 3.12 MiB (`.espdl`) |
| **独立测试集 mAP@0.5** | **0.413506** |
| **独立测试集 mAP@0.5:0.95** | **0.201516** |
| **Precision** | **0.611435** |
| **Recall** | **0.399792** |
| 推理时间 | ~4.45s (ESP32-S3，实测) |

以上为 `train_cloud_20260719` 在无数据泄漏独立测试集（302张、4440个实例）
上的结果。该版本已完成 ESP-DL INT8 量化和板端自检，但整体召回率仍偏低，适合项目
闭环演示和后续数据迭代，不应作为高可靠生产模型宣传。

### 训练配置 (V6)

| 参数 | 值 | 说明 |
|------|-----|------|
| cls 权重 | 1.25 | 高分类权重，降误判 |
| dropout | 0.18 | 强正则化 |
| label_smoothing | 0.06 | 防止过度自信 |
| mosaic | 0.15 | 低值保护小目标 |
| copy_paste | 0.35 | 小目标增广 |

---

## HTTP 端点

| 端点 | 端口 | 说明 |
|------|------|------|
| `/` | 80 | HTML 仪表盘 (双画面) |
| `/stream` | 80 | MJPEG 视频流 (原始画面) |
| `/annotated` | 8080 | 检测标注 JPEG |

### 使用方式

```
浏览器访问: http://<设备IP>/
```

仪表盘显示：
- 左侧：实时画面
- 右侧：检测结果 (每 2s 刷新)

---

## 项目结构

```
esp32s3-cam/
├── main/
│   ├── main_app.cpp              # 主入口
│   ├── cam/
│   │   └── camera.c              # OV5640 驱动
│   ├── wifi/
│   │   └── wifi.c                # WiFi STA 连接
│   ├── http_stream/
│   │   └── http_stream.c         # HTTP 服务器 (双缓冲 MJPEG)
│   ├── yolo_detect/
│   │   ├── yolo_detect.cpp       # YOLO 推理 + DFL 解码 + NMS
│   │   └── yolo_detect.hpp       # 配置 (conf=0.20, iou=0.50)
│   ├── inference_task/
│   │   └── inference_task.cpp    # FreeRTOS 推理任务
│   ├── model/
│   │   └── yolo11n_esp32s3.espdl # INT8 量化模型
│   ├── oled/                     # SSD1306 驱动
│   ├── sht20/                    # 温湿度传感器
│   ├── tsl2584/                  # 光照传感器
│   ├── light_sensor/             # 模拟光敏模块 (ADC)
│   ├── adc1_shared/              # ADC1 共享访问层
│   ├── max471/                   # 电压检测 (ADC)
│   ├── I2C.c                     # I2C 总线初始化
│   └── blink/                    # LED 状态指示
├── yolov11/                      # 模型训练
│   ├── train_and_export.py       # 训练 + ONNX 导出 + 量化
│   ├── data.yaml                 # 数据集配置
│   ├── train/                    # 训练集
│   ├── val/                      # 验证集
│   └── test/                     # 测试集
├── sdkconfig.defaults            # ESP-IDF 配置 (DMA buffer 等)
├── partitions.csv                # Flash 分区表
└── CMakeLists.txt
```

---

## 数据流

```
OV5640 摄像头 (320×320 JPEG)
    │
    ├─► /stream (端口 80)
    │       └─► 双缓冲 PSRAM → MJPEG 流 (零拷贝)
    │
    ├─► /annotated (端口 8080)
    │       └─► 检测标注图 (JPEG)
    │
    └─► 推理管线
            │
            ├─► fmt2rgb888 (JPEG 解码)
            ├─► Letterbox (640×640 填充)
            ├─► HWC→CHW 转置 + INT8 量化
            ├─► ESP-DL 推理 (~4.6s, 双核)
            └─► DFL 解码 + NMS (conf=0.20, iou=0.50)
```

---

## 构建与烧录

### 前提条件

- ESP-IDF v5.4+
- Python 3.8+ (训练模型)
- CUDA GPU (可选，用于训练)

### Wi-Fi 配置

复制 `main/wifi/wifi_config.example.h` 为
`main/wifi/wifi_config.local.h`，然后在本地文件中填写 Wi-Fi 名称和密码。
`wifi_config.local.h` 已被 Git 忽略，不会提交到远端仓库。

### 编译烧录

```bash
# 清理 (首次或配置变更后)
idf.py fullclean

# 编译
idf.py build

# 烧录并监控
idf.py -p COM<X> flash monitor
```

### 串口监控

按 `Ctrl+]` 退出监控。

---

## 模型训练

### 数据集准备

```bash
cd yolov11

# 按 Roboflow 原图 ID 分组，生成 70/15/15 无泄漏清单
python split_dataset.py

# 为每类生成 50 个标注框的人工审计联系表
python audit_labels.py
```

`split_dataset.py` 不移动原始图片和标签。训练、验证、测试清单写入
`splits/`，同一原图的 Roboflow 变体只会出现在一个集合中。人工审计
输出写入 `audit/`，该目录不会提交到 Git。

OV5640 实拍数据应单独保存在 `device_test/images/` 和
`device_test/labels/`，不要混入网络图片测试集。正式指标同时报告独立
测试集结果和真实设备场景结果。

### 云端训练 + 本地导出量化

```bash
# AutoDL GPU
python train_and_export.py train
python train_and_export.py validate --weights runs/detect/train_cloud/weights/best.pt --split val

# 本地 CPU
python train_and_export.py export-onnx --weights best.pt
python train_and_export.py quantize
python train_and_export.py deploy
```

脚本使用显式子命令，避免云端训练环境与 ESP-PPQ 量化环境互相污染。
完整上传清单、AutoDL命令、结果下载清单和本地虚拟环境配置见
[`ziliao/AutoDL云训练与本地ESP-DL量化指南-2026-07-19.md`](ziliao/AutoDL云训练与本地ESP-DL量化指南-2026-07-19.md)。

云端训练前先确认 CUDA 可用：

```bash
nvidia-smi
python -c "import torch; print(torch.cuda.is_available(), torch.cuda.get_device_name(0))"
```

---

## 推理参数调优

### 置信度阈值

编辑 `main/yolo_detect/yolo_detect.hpp`:

```cpp
#define YOLO_CONF_THRESHOLD 0.20f  // 检测置信度阈值
#define YOLO_IOU_THRESHOLD 0.50f   // NMS IoU 阈值
```

| conf 值 | 效果 |
|---------|------|
| 0.15 | 宽松，可能误判多 |
| 0.20 | 平衡 (推荐) |
| 0.25+ | 严格，可能漏检 |

---

## 摄像头配置

编辑 `main/cam/camera.c`:

```c
camera_config_t camera_config = {
    .jpeg_quality = 10,    // JPEG 质量 (1-63, 越小质量越高)
    .fb_count = 3,         // 帧缓冲数量
    // ...
};
```

### DMA Buffer 配置

编辑 `sdkconfig.defaults`:

```
CONFIG_CAMERA_DMA_BUFFER_SIZE_MAX=65536
CONFIG_CAMERA_JPEG_MODE_FRAME_SIZE_CUSTOM=y
CONFIG_CAMERA_JPEG_MODE_FRAME_BUFFER_SIZE=65536
```

---

## 依赖组件

| 组件 | 版本 | 用途 |
|------|------|------|
| espressif/esp-dl | 3.3.5 | 深度学习推理、标量参数加载修复 |
| espressif/esp32-camera | 2.0.15 | 摄像头驱动 |
| esp-ppq | 1.3.6 | 模型量化 |
| ultralytics | 8.4.72 | YOLO 训练与验证 |

---

## 常见问题

### 1. FB-OVF (帧缓冲溢出)

**原因**：JPEG 输出超过 DMA buffer 大小

**解决**：
- 降低 `jpeg_quality`
- 增加 `fb_count`
- 确保 `sdkconfig.defaults` 中 DMA buffer 配置生效

### 2. 检测误判多

**解决**：
- 提高 `YOLO_CONF_THRESHOLD` (如 0.25)
- 使用 V6 模型 (高 Precision)

### 3. 检测漏检

**解决**：
- 降低 `YOLO_CONF_THRESHOLD` (如 0.15)
- 使用 V5 模型 (高 Recall)

### 4. 推理速度慢

**正常**：ESP32-S3 @ 240MHz 双核，YOLO11n 320×320 推理约 4.6s

---

## 版本历史

| 版本 | mAP@0.5 | Precision | Recall | 说明 |
|------|---------|-----------|--------|------|
| V3 | 0.573 | 0.734 | 0.646 | 基准版本 |
| V5 | 0.584 | 0.676 | 0.635 | 训练稳定性改善 |
| V6 | 0.577 | 0.715 | 0.576 | 旧划分下的降误判版本 |
| **Cloud INT8 V1.0** | **0.414** | **0.611** | **0.400** | **无泄漏测试集，当前板端版本** |

版本发布记录、模型哈希和验证证据见 [`CHANGELOG.md`](CHANGELOG.md)。

---

## License

MIT License
