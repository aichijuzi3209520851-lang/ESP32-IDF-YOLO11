# 双模型云端训练完整操作指南

> 适用日期：2026-07-24
> 算力平台：AutoDL (RTX 5090)
> 本地目录：`D:\ESP32-IDF\esp32s3-cam\yolov11\`

---

## 一、模型概述

| | ESP32 轻量模型 | PC 高精度模型 |
|---|---|---|
| 模型 | YOLO11n | YOLO11s |
| 输入尺寸 | 320×320 | 640×640 |
| 类别数 | 2 | 5 |
| 类别 | stain, damage | bird_drop, dust, electrical_damage, physical_damage, snow_covered |
| 部署方式 | ESP-DL INT8 量化 | 本地 FP32 ONNX |
| 训练设备 | AutoDL RTX 5090 | AutoDL RTX 5090 |
| 目标 mAP50 | ≥ 70% | ≥ 70% |

---

## 二、本地操作步骤

### 步骤 1：生成双数据集

打开 PowerShell 或 CMD：

```powershell
cd D:\ESP32-IDF\esp32s3-cam\yolov11
python generate_dual_datasets.py
```

**预期输出**：
```
============================================================
  双模型数据集生成器
============================================================
  原始数据: D:\ESP32-IDF\esp32s3-cam\yolov11
  原始类别: 6类

============================================================
  生成 ESP32-2class 数据集
============================================================
  train: 1420 images
  val: 303 images
  test: 302 images

  --- ESP32-2class 数据集统计 ---
  train: XXXX images (排除XX, 负样本XX)
    boxes: stain=XXXX, damage=XXXX
  val: ...
  test: ...

============================================================
  生成 PC-5class 数据集
============================================================
  ...
```

> **截图 1**：截取数据集生成完成的终端输出，显示各 split 的图片数量和标注框统计。

---

### 步骤 2：打包 ESP32 训练包

```powershell
cd D:\ESP32-IDF\esp32s3-cam\yolov11
python pack_for_cloud.py esp32
```

输出文件在 `yolov11\cloud_packages\yolo11-esp32_2class-cloud.tar.gz`

**如果文件超过 450MB**，终端会提示分卷命令，按提示执行。

> **截图 2**：截取打包完成信息，显示文件大小和 SHA256。

---

### 步骤 3：打包 PC 训练包

```powershell
python pack_for_cloud.py pc
```

> **截图 3**：截取 PC 训练包打包完成信息。

---

## 三、AutoDL 云端训练操作

### 3.1 租用实例

1. 打开 [AutoDL 官网](https://www.autodl.com)
2. 选择 **RTX 5090** 显卡
3. 镜像选择：**PyTorch 2.8.0 + CUDA 12.8 + Python 3.12**
4. 数据盘大小建议：**30GB 以上**
5. 创建实例

> **截图 4**：截取 AutoDL 实例配置页面（显示 GPU 型号、镜像、磁盘大小）。

---

### 3.2 上传训练包

方法一：使用 AutoDL 网页上传

1. 打开实例的 **JupyterLab** 或 **文件管理**
2. 上传 `yolo11-esp32_2class-cloud.tar.gz` 到 `/root/autodl-tmp/`
3. 同样上传 `yolo11-pc_5class-cloud.tar.gz`

方法二：使用 AutoDL 提供的网盘传输（如果文件太大）

> **截图 5**：截取上传完成后的文件列表，显示两个 tar.gz 文件。

---

### 3.3 解压数据集

在 AutoDL 终端中执行：

```bash
cd /root/autodl-tmp

# 解压 ESP32 数据集
tar -xzf yolo11-esp32_2class-cloud.tar.gz

# 解压 PC 数据集
tar -xzf yolo11-pc_5class-cloud.tar.gz

# 合并目录
mkdir -p yolov11/datasets
cp -r yolo11-esp32_2class-cloud/datasets/esp32_2class yolov11/datasets/
cp -r yolo11-pc_5class-cloud/datasets/pc_5class yolov11/datasets/
cp yolo11-esp32_2class-cloud/train_cloud.py yolov11/
cp yolo11-esp32_2class-cloud/requirements-cloud.txt yolov11/
cp yolo11-esp32_2class-cloud/audit_labels.py yolov11/

# 安装依赖
cd yolov11
pip install -r requirements-cloud.txt -i https://pypi.tuna.tsinghua.edu.cn/simple
```

--- 确认数据集结构正确：

```bash
ls datasets/esp32_2class/
# 应看到: data.yaml  train/  val/  test/  splits/

ls datasets/pc_5class/
# 应看到: data.yaml  train/  val/  test/  splits/
```

> **截图 6**：截取 `ls datasets/esp32_2class/` 和 `ls datasets/pc_5class/` 的输出。

---

### 3.4 开始训练 ESP32 模型

```bash
cd /root/autodl-tmp/yolov11
python train_cloud.py esp32
```

训练过程中屏幕会显示：
- 环境检查（GPU 型号、PyTorch 版本、CUDA 版本）
- 每个 epoch 的 loss、mAP 等指标
- 训练进度条

> **截图 7**：截取训练开始时的环境检查信息（GPU 型号、PyTorch 版本）。
> **截图 8**：截取训练进行中某个 epoch 的指标（loss、mAP50、mAP50-95）。
> **截图 9**：截取训练完成后的最终指标（最佳 mAP50、mAP50-95）。
> **截图 10**：截取训练完成后的测试集评估结果（各类别 mAP）。

训练完成后会输出：

```
============================================================
  训练完成！
============================================================
  输出目录:  /root/autodl-tmp/yolov11/runs/esp32/...
  最佳权重:  /root/autodl-tmp/yolov11/runs/esp32/weights/best.pt
  最后权重:  /root/autodl-tmp/yolov11/runs/esp32/weights/last.pt
```

---

### 3.5 开始训练 PC 模型

ESP32 训练完成后，接着训练 PC 模型：

```bash
cd /root/autodl-tmp/yolov11
python train_cloud.py pc
```

> **截图 11**：截取 PC 模型训练开始的环境检查。
> **截图 12**：截取 PC 模型训练完成后的最终指标。
> **截图 13**：截取 PC 模型测试集评估结果。

---

### 3.6 下载训练产物

训练完成后，**在关停 AutoDL 实例之前**，下载以下文件到本地：

**必须下载的文件夹**：

```bash
# 在 AutoDL 终端中先打包
cd /root/autodl-tmp/yolov11
tar -czf esp32_results.tar.gz runs/esp32/
tar -czf pc_results.tar.gz runs/pc/
```

然后在 AutoDL 网页文件管理中下载：
- `esp32_results.tar.gz`
- `pc_results.tar.gz`

**重点确保下载的文件**：
- `runs/esp32/weights/best.pt` — ESP32 最佳权重
- `runs/esp32/weights/last.pt` — ESP32 最后权重（可用于断点续训）
- `runs/esp32/results.csv` — 训练指标 CSV
- `runs/esp32/confusion_matrix.png` — 混淆矩阵
- `runs/esp32/results.png` — 训练曲线图
- `runs/esp32/F1_curve.png` — F1 曲线
- `runs/esp32/PR_curve.png` — PR 曲线
- `runs/esp32/train_batch*.jpg` — 训练 batch 样本
- `runs/esp32/val_batch*.jpg` — 验证 batch 预测结果
- `runs/esp32_test/` — 测试集评估结果（含混淆矩阵、曲线）

**PC 模型同理**，对应的 `runs/pc/` 目录。

> **截图 14**：截取训练曲线图（results.png）— 显示 loss 下降和 mAP 上升。
> **截图 15**：截取混淆矩阵（confusion_matrix.png）。
> **截图 16**：截取验证 batch 预测结果（val_batch*.jpg）— 显示模型在验证集上的实际检测效果。

---

### 3.7 关停实例

下载完成后，**立即关停 AutoDL 实例**，避免继续扣费。

> **截图 17**：截取关停实例确认页面。

---

## 四、本地：下载产物存放

将下载的 `esp32_results.tar.gz` 和 `pc_results.tar.gz` 解压到：

```
D:\ESP32-IDF\esp32s3-cam\yolov11\cloud_results_20260724\
  ├── esp32\
  │   ├── weights\best.pt
  │   ├── weights\last.pt
  │   ├── results.csv
  │   ├── results.png
  │   ├── ...
  │   └── esp32_test\...
  └── pc\
      ├── weights\best.pt
      ├── weights\last.pt
      ├── results.csv
      ├── ...
      └── pc_test\...
```

---

## 五、断点续训（如果训练中断）

如果训练被中断，可以用 `--resume` 恢复：

```bash
cd /root/autodl-tmp/yolov11
python train_cloud.py esp32 --resume
```

前提是 `runs/esp32/weights/last.pt` 存在。

---

## 六、截图汇总清单（写入文档用）

| 编号 | 截图内容 | 用途 |
|---|---|---|
| 1 | 本地数据集生成终端输出 | 证明数据预处理流程 |
| 2 | ESP32 打包完成信息 | 证明训练包完整性 |
| 3 | PC 打包完成信息 | 同上 |
| 4 | AutoDL 实例配置 | 证明算力环境 |
| 5 | 上传文件列表 | 证明数据上传成功 |
| 6 | 数据集目录结构 | 证明数据集结构正确 |
| 7 | ESP32 训练环境检查 | 证明 GPU/CUDA 环境 |
| 8 | ESP32 训练中指标 | 证明训练过程 |
| 9 | ESP32 训练最终指标 | 核心结果 |
| 10 | ESP32 测试集评估 | 独立测试集结果 |
| 11 | PC 训练环境检查 | 证明 GPU/CUDA 环境 |
| 12 | PC 训练最终指标 | 核心结果 |
| 13 | PC 测试集评估 | 独立测试集结果 |
| 14 | 训练曲线图 | 证明收敛过程 |
| 15 | 混淆矩阵 | 证明各类别表现 |
| 16 | 验证 batch 预测 | 视觉效果 |
| 17 | 关停实例 | 证明资源管理 |

**论文/文档中重点展示**：截图 9、10、12、13、14、15、16。

---

## 七、常见问题

### Q: 训练中 mAP 不涨怎么办？
- 检查数据集是否正确解压、路径是否正确
- 检查 `data.yaml` 中的类别数是否匹配
- 尝试降低学习率（`lr0=0.0005`）
- 检查标签是否正确映射

### Q: AutoDL 连接断开怎么办？
- 使用 `--resume` 恢复训练
- 训练不会丢失已完成 epoch 的权重

### Q: 文件太大上传不了？
- 使用分卷功能，`pack_for_cloud.py` 会自动提示分卷命令
- 或使用 AutoDL 的无卡模式开机上传（更便宜）

### Q: 训练一个模型需要多久？
- RTX 5090，YOLO11n 320×320，150 epochs：约 30-60 分钟
- RTX 5090，YOLO11s 640×640，150 epochs：约 1-2 小时