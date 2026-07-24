# ESP32-S3-CAM 项目交接文档

> 本文档用于 Codex 对话上下文丢失后快速恢复项目记忆。
> 最后更新：2026-07-24

---

## 一、项目概述

### 1.1 项目背景
ESP32-S3 太阳能板智能检测相机 — 边缘 AI 设备，集成 YOLO11n INT8 缺陷检测 + MJPEG 视频流 + 环境传感器。检测 6 类太阳能板缺陷（鸟粪/灰尘/积雪/电损伤/物理损伤/清洁）。

### 1.2 双模型架构
项目采用 **ESP32 边缘端 + PC 云端** 双模型策略：

| 维度 | ESP32 轻量模型 | PC 高精度模型 |
|------|---------------|--------------|
| 用途 | 边缘端实时检测 | 云端高精度分析 |
| 模型 | YOLO11n | YOLO11s |
| 输入尺寸 | 320×320 | 640×640 |
| 类别数 | 2 类 | 5 类 |
| 类别 | `stain`(污渍), `damage`(损伤) | `bird_drop`, `dust`, `electrical_damage`, `physical_damage`, `snow_covered` |
| 精度 | INT8 量化 | FP32 |
| Batch | 64 | 32 |
| Epochs | 150 | 150 |

### 1.3 类别映射关系
原始 6 类 → 新数据集映射：

```
原始类别: 0=bird_drop, 1=clean, 2=dust, 3=electrical_damage, 4=physical_damage, 5=snow_covered

ESP32 2类映射:
  bird_drop (0)     → stain (0)
  dust (2)          → stain (0)
  electrical_damage (3) → damage (1)
  physical_damage (4)   → damage (1)
  clean (1)         → 移除标注框（负样本）
  snow_covered (5)  → 排除整张图片

PC 5类映射:
  bird_drop (0)     → bird_drop (0)
  dust (2)          → dust (1)
  electrical_damage (3) → electrical_damage (2)
  physical_damage (4)   → physical_damage (3)
  snow_covered (5)  → snow_covered (4)
  clean (1)         → 移除标注框（负样本）
```

---

## 二、项目目录结构

### 2.1 核心目录
```
d:\ESP32-IDF\esp32s3-cam\
├── main/                          # 固件主代码
│   ├── cam/camera.c               # OV5640 摄像头驱动
│   ├── wifi/wifi.c                # WiFi 连接
│   ├── http_stream/               # HTTP 视频流服务器
│   ├── yolo_detect/               # YOLO 推理（ESP-DL）
│   ├── inference_task/            # 推理 FreeRTOS 任务
│   ├── sensor_data/               # 传感器数据管理
│   ├── model/                     # 嵌入的 .espdl 模型
│   ├── dashboard.html             # Web 仪表盘
│   └── yolo_classes.h             # 6类缺陷业务逻辑
├── yolov11/                       # 模型训练相关（重点！）
│   ├── generate_dual_datasets.py  # 生成双数据集
│   ├── pack_for_cloud.py          # 打包上传到 AutoDL
│   ├── train_cloud.py             # 云端训练脚本
│   ├── deploy_models.py           # 模型导出与部署
│   ├── split_dataset.py           # 数据集划分
│   ├── audit_labels.py            # 标签审计
│   ├── data.yaml                  # 原始 6 类数据配置
│   ├── datasets/                  # 生成的双数据集（本地）
│   │   ├── esp32_2class/
│   │   └── pc_5class/
│   ├── cloud_packages/            # 打包好的上传包
│   └── requirements-cloud.txt     # 云端依赖
├── components/ssd1306/            # OLED 驱动
├── ziliao/                        # 项目文档资料 + 训练结果
│   ├── 全国大学生物联网竞赛文档模板.docx
│   ├── esp32_results.tar.gz       # ESP32 训练结果（已下载）
│   ├── pc_results.tar.gz          # PC 训练结果（已下载）
│   └── 各种文档说明...
└── AGENTS.md                      # 项目说明文档
```

### 2.2 关键脚本说明

| 脚本 | 用途 | 运行位置 |
|------|------|---------|
| `generate_dual_datasets.py` | 从原始 6 类数据生成 ESP32 2类 + PC 5类 两套数据集 | 本地 |
| `pack_for_cloud.py esp32/pc` | 将数据集+脚本打包成 tar.gz 用于上传 | 本地 |
| `train_cloud.py esp32/pc` | 云端训练主脚本（环境检查→训练→测试） | AutoDL 云端 |
| `deploy_models.py` | 模型导出、量化、部署到固件 | 本地 |
| `split_dataset.py` | 原始数据集按 70/15/15 划分 | 本地 |
| `audit_labels.py` | 标签文件审计 | 本地/云端 |

---

## 三、当前进度

### 3.1 已完成工作

#### ✅ 本地数据准备
- [x] 原始数据集划分（train 1420 / val 303 / test 302）
- [x] 双数据集生成：
  - ESP32 2类：train 968 / val 206 / test 206
  - PC 5类：train 1420 / val 303 / test 302
- [x] 两个训练包打包完成：
  - ESP32包：125.3 MB
  - PC包：179.4 MB

#### ✅ 云端环境配置（AutoDL）
- GPU：RTX 5090 32GB
- PyTorch：2.8.0 + CUDA 12.8
- Python：3.12.3
- 数据盘：50GB SSD
- 工作目录：`/root/autodl-tmp/yolov11/`

#### ✅ ESP32 模型训练完成
- **训练参数**：YOLO11n, 320×320, batch 64, 150 epochs, AdamW, amp=False
- **验证集最佳指标**（best.pt）：
  - Precision: 53.6%
  - Recall: 43.0%
  - mAP50: **41.1%**
  - mAP50-95: 21.9%
  - stain mAP50: 25.3%
  - damage mAP50: 56.9%
- **测试集指标**：
  - Precision: 54.81%
  - Recall: 37.60%
  - mAP50: 34.44%
  - mAP50-95: 15.53%
- **结果位置**：`/root/autodl-tmp/yolov11/runs/esp32/`
- **测试评估位置**：`/root/autodl-tmp/yolov11/runs/esp32_test/`
- **备份文件**：`/root/autodl-tmp/esp32_results.tar.gz`（293 MB）
- **本地备份**：`D:\ESP32-IDF\esp32s3-cam\ziliao\esp32_results.tar.gz`
- **训练日志**：`/root/autodl-tmp/yolov11/esp32_training_retry.log`

#### ✅ ESP32 模型分析结论
- 训练曲线正常，无明显过拟合
- 主要问题：**stain 类漏检严重**（Recall 低）
  - 原因1：320×320 下小污渍目标特征丢失
  - 原因2：stain 合并了鸟粪+灰尘，类内差异大
  - 原因3：训练集标注框 stain:damage ≈ 8:1，类别不均衡
  - 原因4：光伏板纹理、反光容易误识别为 stain
- 混淆矩阵：主要是漏检（被识别为背景），类别互相混淆较少
- mAP50 41.1% 作为基线可接受，后续需优化

#### ✅ PC 模型训练完成
- **训练参数**：YOLO11s, 640×640, batch 32, 150 epochs, AdamW, amp=False
- **验证集最佳指标**（best.pt）：
  - Precision: 57.5%
  - Recall: 48.1%
  - mAP50: **46.9%**
  - mAP50-95: 29.1%
  - electrical_damage mAP50: 61.7%（最好）
  - bird_drop mAP50: 37.6%（最差）
- **测试集指标**：
  - Precision: 71.34%
  - Recall: 46.00%
  - mAP50: 49.33%
  - mAP50-95: 30.15%
- **结果位置**：`/root/autodl-tmp/yolov11/runs/pc/`
- **测试评估位置**：`/root/autodl-tmp/yolov11/runs/pc_test/`
- **备份文件**：`/root/autodl-tmp/pc_results.tar.gz`（1.1 GB）
- **本地备份**：`D:\ESP32-IDF\esp32s3-cam\ziliao\pc_results.tar.gz`
- **训练日志**：`/root/autodl-tmp/yolov11/pc_training.log`

#### ✅ PC 模型分析结论
- 训练曲线正常，无明显过拟合
- 主要问题：漏检严重（约 48~57% 目标被识别为背景）
- 类别混淆较少（各类之间误分类仅 1~3%）
- 误报较多：光伏板纹理、反光容易被识别为 bird_drop

#### ✅ 结果文件已下载到本地
- ESP32 结果：`D:\ESP32-IDF\esp32s3-cam\ziliao\esp32_results.tar.gz`（293 MB）
- PC 结果：`D:\ESP32-IDF\esp32s3-cam\ziliao\pc_results.tar.gz`（1.1 GB）

### 3.2 待完成工作
- [ ] 解压本地结果文件，确认 best.pt 等文件完整
- [ ] 整理截图到统一文件夹（方便写竞赛文档）
- [ ] ESP32 模型导出 ONNX → INT8 量化 → 部署到固件
- [ ] PC 模型导出 ONNX
- [ ] 固件编译验证（替换新模型后）
- [ ] 关停 AutoDL 实例（确认结果没问题后）
- [ ] 竞赛文档撰写

---

## 四、AutoDL 云端环境详情

### 4.1 目录结构
```
/root/autodl-tmp/
├── yolov11/
│   ├── train_cloud.py            # 训练脚本（已修正路径和amp=False）
│   ├── requirements-cloud.txt
│   ├── audit_labels.py
│   ├── yolo11n.pt                # 预训练权重（已上传）
│   ├── yolo11s.pt                # 预训练权重（已上传）
│   ├── esp32_training_retry.log  # ESP32 训练日志
│   ├── pc_training.log           # PC 训练日志
│   ├── datasets/
│   │   ├── esp32_2class/
│   │   │   └── data.yaml         # 已修正为绝对路径 + train/images
│   │   └── pc_5class/
│   │       └── data.yaml         # 已修正为绝对路径 + train/images
│   └── runs/
│       ├── esp32/                # ESP32 训练结果（best.pt 等）
│       ├── esp32_test/           # ESP32 测试评估结果
│       ├── pc/                   # PC 训练结果（best.pt 等）
│       └── pc_test/              # PC 测试评估结果
├── esp32_results.tar.gz          # ESP32 结果备份
├── pc_results.tar.gz             # PC 结果备份
├── yolo11-esp32_2class-cloud.tar.gz   # 原始上传包
└── yolo11-pc_5class-cloud.tar.gz      # 原始上传包
```

### 4.2 云端 data.yaml 修正（重要！）
原始生成的 data.yaml 有路径问题，**云端已手动修正**，本地也已修正：

**修正前（错误）：**
```yaml
path: .
train: splits/train.txt
val: splits/val.txt
test: splits/test.txt
```

**修正后（正确）：**
```yaml
path: /root/autodl-tmp/yolov11/datasets/esp32_2class
train: train/images
val: val/images
test: test/images
```

> ⚠️ 注意：如果重新生成数据集，需要重新修正 data.yaml 的 path 和 train/val/test 路径！

### 4.3 train_cloud.py 已修正项
1. `output_dir` 路径：`runs/esp32_2class` → `runs/esp32`，`runs/pc_5class` → `runs/pc`
2. `amp=False`：关闭混合精度训练（避免卡在 AMP 检查）
3. 本地和云端脚本都已同步修正

---

## 五、重要截图清单（竞赛文档用）

### 全部 15 张截图（已完成）
| 序号 | 文件名 | 说明 | 状态 |
|------|--------|------|------|
| 01 | `01_本地双数据集生成统计.png` | 本地数据集生成统计 | ✅ |
| 02 | `02_ESP32训练包打包完成.png` | ESP32 训练包大小+SHA256 | ✅ |
| 03 | `03_PC训练包打包完成.png` | PC 训练包大小+SHA256 | ✅ |
| 04 | `04_AutoDL实例配置.png` | AutoDL 实例配置信息 | ✅ |
| 05 | `05_ESP32训练环境检查.png` | Python/PyTorch/CUDA/GPU/数据集 | ✅ |
| 06 | `06_ESP32训练最终指标.png` | 验证集 best.pt 指标 | ✅ |
| 07 | `07_ESP32测试集评估结果.png` | 测试集 Precision/Recall/mAP | ✅ |
| 08 | `08_ESP32模型训练曲线.png` | results.png 训练曲线 | ✅ |
| 09 | `09_ESP32测试集混淆矩阵.png` | confusion_matrix_normalized.png | ✅ |
| 10 | `10_ESP32测试集检测效果.png` | val_batch0_pred.jpg 检测效果图 | ✅ |
| 11 | `11_PC训练最终指标.png` | 验证集 best.pt 指标 | ✅ |
| 12 | `12_PC测试集评估结果.png` | 测试集评估结果 | ✅ |
| 13 | `13_PC模型训练曲线.png` | results.png | ✅ |
| 14 | `14_PC测试集混淆矩阵.png` | 混淆矩阵 | ✅ |
| 15 | `15_PC测试集检测效果.png` | val_batch0_pred.jpg | ✅ |

> 截图保存在用户本地，按序号命名，方便按顺序插入竞赛文档。

---

## 六、已知问题与注意事项

### 6.1 GitHub 下载不稳定
- 问题：AutoDL 环境下载 GitHub 上的预训练权重容易超时断连
- 解决：手动上传本地的 `yolo11n.pt` 和 `yolo11s.pt` 到 `/root/autodl-tmp/yolov11/`
- 状态：两个权重均已上传

### 6.2 AMP 检查卡住
- 问题：`AMP: running Automatic Mixed Precision (AMP) checks...` 长时间无响应
- 解决：设置 `amp=False` 关闭混合精度训练
- 影响：训练速度可能略慢，显存占用略高，但 RTX 5090 32GB 足够
- 状态：已在 `train_cloud.py` 中修正

### 6.3 data.yaml 路径问题
- 问题：`path: .` 被 Ultralytics 解析到项目根目录，导致找不到图片
- 解决：使用绝对路径，并将 train/val/test 直接指向 `train/images` 目录
- 状态：云端两份 data.yaml 已修正，本地生成脚本也已修正

### 6.4 模型精度偏低
- ESP32 mAP50 ≈ 41.1%，PC mAP50 ≈ 49.3%，均未达到 70% 目标
- 主要原因：
  - 小目标多，低分辨率下特征丢失
  - 类别不均衡（stain/dust 类标注框远多于其他类）
  - 光伏板纹理、反光造成误报
  - 漏检严重（大量目标被识别为背景）
- 后续优化方向：
  1. 提高输入尺寸
  2. 类别均衡采样
  3. 数据增强调整
  4. 切图检测（针对小目标）
- 当前策略：先完成基线，后统一优化

### 6.5 训练失败的输出目录处理
- 每次失败后需要将不完整的 `runs/esp32` 或 `runs/pc` 移走
- 否则 Ultralytics 会自动命名为 `esp322`、`esp323` 等，导致脚本找不到权重
- 处理命令：
  ```bash
  if [ -d runs/esp32 ]; then
      mv runs/esp32 "runs/esp32_failed_$(date +%H%M%S)"
  fi
  ```

---

## 七、下一步操作指南

### 7.1 解压结果文件（本地）
```powershell
cd D:\ESP32-IDF\esp32s3-cam\ziliao

# 解压 ESP32 结果
tar -xzf esp32_results.tar.gz

# 解压 PC 结果
tar -xzf pc_results.tar.gz

# 确认 best.pt 文件存在
dir runs\esp32\weights\best.pt
dir runs\pc\weights\best.pt
```

### 7.2 ESP32 模型导出与部署
```powershell
cd D:\ESP32-IDF\esp32s3-cam\yolov11

# 1. 导出 ONNX（ESP-DL 兼容格式，六输出头）
python deploy_models.py export-esp32 --weights ..\ziliao\runs\esp32\weights\best.pt

# 2. INT8 量化（生成 .espdl）
python deploy_models.py quantize-esp32

# 3. 部署到固件 main/model/
python deploy_models.py deploy-esp32

# 4. 重新编译固件
cd ..
idf.py build
```

### 7.3 PC 模型导出
```powershell
cd D:\ESP32-IDF\esp32s3-cam\yolov11

# 导出 ONNX（FP32，直接使用）
python deploy_models.py export-pc --weights ..\ziliao\runs\pc\weights\best.pt
```

### 7.4 关停 AutoDL 实例
确认本地所有结果完整、模型部署没问题后，再关停实例。

> ⚠️ **重要**：关停前务必确认 best.pt 等关键文件已下载且本地可用！

### 7.5 整理截图
将 15 张截图统一放到一个文件夹，按序号命名，方便写竞赛文档时插入。

---

## 八、常用命令速查

### 本地命令
```powershell
# 进入项目目录
cd D:\ESP32-IDF\esp32s3-cam\yolov11

# 生成双数据集
python generate_dual_datasets.py

# 打包训练包
python pack_for_cloud.py esp32
python pack_for_cloud.py pc

# 标签审计
python audit_labels.py datasets/esp32_2class

# ESP32 模型部署全套
python deploy_models.py export-esp32 --weights <path/to/best.pt>
python deploy_models.py quantize-esp32
python deploy_models.py deploy-esp32

# PC 模型导出
python deploy_models.py export-pc --weights <path/to/best.pt>
```

### 云端命令
```bash
# 进入工作目录
cd /root/autodl-tmp/yolov11

# 环境检查
python train_cloud.py esp32 --check

# 开始训练（带日志记录）
python train_cloud.py esp32 2>&1 | tee esp32_training.log
python train_cloud.py pc 2>&1 | tee pc_training.log

# 断点续训
python train_cloud.py esp32 --resume

# 仅测试评估
python train_cloud.py esp32 --validate-only

# 查看日志末尾
tail -n 50 pc_training.log

# 搜索错误
grep -nE 'Traceback|ERROR|Error' pc_training.log | tail -n 20

# 检查文件数量
find datasets/esp32_2class/train/images -maxdepth 1 -type f | wc -l

# 清理缓存
find datasets -type f -name '*.cache' -delete

# 打包结果
tar -czf /root/autodl-tmp/esp32_results.tar.gz runs/esp32 runs/esp32_test esp32_training_retry.log
tar -czf /root/autodl-tmp/pc_results.tar.gz runs/pc runs/pc_test pc_training.log
```

### 固件编译命令
```powershell
cd D:\ESP32-IDF\esp32s3-cam

# 清理重建
idf.py fullclean

# 编译
idf.py build

# 烧录+监控
idf.py -p COM<X> flash monitor
```

---

## 九、竞赛文档模板说明

文档模板位置：`D:\ESP32-IDF\esp32s3-cam\ziliao\全国大学生物联网竞赛文档模板.docx`

- 格式：A4 纵向，9 个分节
- 包含：封面、填写说明、摘要、目录、正文三级标题体系
- 训练截图将用于"系统设计"、"实验结果"等章节
- 截图已按序号命名，方便按顺序插入文档

### 其他参考文档（ziliao/ 目录下）
- `AI交接说明-竞赛项目上下文.md`
- `AutoDL云训练与本地ESP-DL量化指南-2026-07-19.md`
- `DHT11温湿度传感器问题分析与接线指南.md`
- `YOLO类别统一与数据修复操作记录-2026-07-19.md`
- `竞赛项目重定位与开发计划.md`
- `项目全流程架构解读.md`

---

## 十、快速恢复 checklist

新对话开始后，按以下顺序确认：

- [ ] 读取本交接文档
- [ ] 确认当前工作阶段（模型训练已完成，待部署）
- [ ] 检查 ziliao/ 目录下的结果文件是否存在
- [ ] 解压结果文件，确认 best.pt 完整
- [ ] ESP32 模型导出 ONNX → INT8 量化 → 部署固件
- [ ] PC 模型导出 ONNX
- [ ] 固件编译验证
- [ ] 整理截图，准备竞赛文档
- [ ] 确认无误后关停 AutoDL 实例

---

*本文档最后更新：2026-07-24*
