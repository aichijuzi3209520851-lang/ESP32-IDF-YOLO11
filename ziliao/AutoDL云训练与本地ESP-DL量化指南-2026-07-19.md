# AutoDL云训练与本地ESP-DL量化指南

日期：2026-07-19

## 1. 最终流程

本项目不再在同一个Python进程中连续执行训练、ONNX导出和ESP-DL量化。
新流程分为两套环境：

```text
AutoDL RTX 5090
  -> 训练YOLO11n
  -> 验证集选择best.pt
  -> 独立测试集只评估一次
  -> 下载完整训练结果

本地Windows CPU
  -> best.pt导出ESP-DL兼容ONNX
  -> ESP-PPQ进行INT8量化
  -> 生成.espdl
  -> 复制进ESP32-IDF工程
  -> 编译、烧录、板端自检
```

云端不安装ESP-PPQ，本地不进行耗时训练。量化使用CPU即可，AMD核显不影响。

## 2. AutoDL实例配置

推荐配置：

```text
GPU：RTX 5090 32GB x 1
计费：按量计费
镜像：PyTorch 2.8 + CUDA 12.8 + Ubuntu 22.04
Python：3.10、3.11或3.12均可用于云端训练
数据盘：免费50GB，不扩容
```

不要为RTX 5090选择PyTorch 2.1 + CUDA 12.1。Blackwell架构应使用
PyTorch 2.7以上及CUDA 12.8运行时。

## 3. 需要上传的文件

保持以下目录结构完整：

```text
yolov11/
├── data.yaml
├── split_dataset.py
├── train_and_export.py
├── requirements-cloud.txt
├── yolo11n.pt
├── splits/
│   ├── train.txt
│   ├── val.txt
│   └── test.txt
├── train/
│   ├── images/    1620张
│   └── labels/    1620个
└── val/
    ├── images/     405张
    └── labels/     405个
```

说明：物理文件仍保存在原Roboflow的 `train/` 和 `val/` 目录中，但训练实际使用
`splits/*.txt`。三份清单已经按原图来源重新分组，同一原图的增强版本不会跨集合。

不需要上传：

```text
build/
main/
managed_components/
yolov11/runs/
yolov11/audit/
论文和竞赛资料
旧的.espdl模型
```

在项目根目录生成标准 `tar.gz` 上传包：

```powershell
tar --exclude="*.cache" -czf yolov11-cloud-upload-v2.tar.gz `
  yolov11/data.yaml `
  yolov11/split_dataset.py `
  yolov11/train_and_export.py `
  yolov11/requirements-cloud.txt `
  yolov11/yolo11n.pt `
  yolov11/splits `
  yolov11/train `
  yolov11/val
```

当前已将归档切分为六个小分卷，避免JupyterLab上传大文件时重复1MiB数据块：

```text
yolov11-cloud-upload-v2.tar.gz.part01
yolov11-cloud-upload-v2.tar.gz.part02
yolov11-cloud-upload-v2.tar.gz.part03
yolov11-cloud-upload-v2.tar.gz.part04
yolov11-cloud-upload-v2.tar.gz.part05
yolov11-cloud-upload-v2.tar.gz.part06
```

归档及分卷只用于上传，不提交Git。

## 4. 开机后先验证环境

将六个分卷上传到AutoDL数据盘 `/root/autodl-tmp/`，校验后合并并解压：

```bash
cd /root/autodl-tmp
sha256sum yolov11-cloud-upload-v2.tar.gz.part*
cat yolov11-cloud-upload-v2.tar.gz.part* > yolov11-cloud-upload-v2.tar.gz
sha256sum yolov11-cloud-upload-v2.tar.gz
tar -xzf yolov11-cloud-upload-v2.tar.gz
cd yolov11
```

完整归档SHA256应为：

```text
81c44d201e69ce0ee2daf7a28d96e359b4762b084664a237dabcb12b1012e5b2
```

先检查GPU，不要立即安装大量软件：

```bash
nvidia-smi
python -c "import torch; print('torch=', torch.__version__); print('cuda=', torch.version.cuda); print('available=', torch.cuda.is_available()); print('gpu=', torch.cuda.get_device_name(0))"
```

必须看到：

```text
torch 2.7或更高
CUDA 12.8或更高
available=True
NVIDIA GeForce RTX 5090
```

如果出现 `sm_120 is not compatible`，不要训练，立即关机并更换镜像。

安装训练依赖：

```bash
python -m pip install -r requirements-cloud.txt
```

`requirements-cloud.txt`故意不包含PyTorch，防止pip覆盖镜像自带的CUDA 12.8版本。

## 5. 验证上传数据

执行确定性数据检查并重新生成相同清单：

```bash
python split_dataset.py
wc -l splits/train.txt splits/val.txt splits/test.txt
```

预期结果：

```text
train.txt  1420
val.txt     303
test.txt    302
总计       2025
```

脚本还应报告三个集合的原图来源交集为零；任何缺图、缺标签、非法类别ID或越界坐标
都会直接报错。

## 6. 一轮冒烟测试

正式计费训练前先跑1轮，验证CUDA、数据路径和输出目录：

```bash
python train_and_export.py train \
  --epochs 1 \
  --batch 32 \
  --workers 4 \
  --name smoke_5090
```

确认日志中使用 `CUDA:0`，并生成：

```text
runs/detect/smoke_5090/weights/best.pt
```

如果日志显示CPU，立即停止，不要继续付费训练。

## 7. 今晚正式训练

推荐只先运行一组可靠配置，避免在未看结果前盲目烧多组超参数：

```bash
python train_and_export.py train \
  --epochs 120 \
  --batch 64 \
  --workers 4 \
  --patience 25 \
  --name train_cloud_20260719 \
  2>&1 | tee train_cloud_20260719.log
```

使用120轮上限和25轮早停，而不是认定轮数越多越好。脚本保持320x320输入、
YOLO11n预训练权重、AMP混合精度和当前项目的低误判训练参数。

训练意外中断时，使用同一目录中的 `last.pt` 恢复：

```bash
python train_and_export.py train \
  --model runs/detect/train_cloud_20260719/weights/last.pt \
  --resume
```

不要用 `best.pt` 代替 `last.pt` 恢复优化器状态。

## 8. 如何确定最佳模型

训练过程每轮使用验证集，Ultralytics将验证适应度最高的权重保存为 `best.pt`。
正式训练完成后先运行验证集评估：

```bash
python train_and_export.py validate \
  --weights runs/detect/train_cloud_20260719/weights/best.pt \
  --split val
```

重点检查：

```text
mAP50-95
mAP50
Precision
Recall
六个类别的逐类mAP50-95
混淆矩阵
PR曲线
```

不能只看总mAP。项目安全决策尤其需要检查 `electrical_damage`、
`physical_damage` 和 `snow_covered` 是否被误判成可清洁类别。

只有确定不再调整模型后，才对独立测试集评估一次：

```bash
python train_and_export.py validate \
  --weights runs/detect/train_cloud_20260719/weights/best.pt \
  --split test
```

不要根据测试集反复调参，否则测试集会变成另一个验证集，最终指标失去独立性。

第一轮训练结束后，应先分析 `results.csv`、`results.png`、混淆矩阵和逐类指标，
再决定是否值得进行第二组训练。不能保证单靠更贵GPU或更多轮数得到“最佳识别效果”。

## 9. 关机前必须下载

至少下载以下内容：

```text
runs/detect/train_cloud_20260719/                 整个目录
runs/detect/test_best/或test_best同类评估目录       测试结果
train_cloud_20260719.log                           终端日志
```

整个训练目录应包含：

```text
weights/best.pt
weights/last.pt
args.yaml
results.csv
results.png
confusion_matrix.png
confusion_matrix_normalized.png
BoxPR_curve.png
BoxP_curve.png
BoxR_curve.png
BoxF1_curve.png
验证批次预测图
```

可以先在云端压缩：

```bash
tar -czf train_cloud_20260719-results.tar.gz \
  runs/detect/train_cloud_20260719 \
  runs/detect/test_best \
  train_cloud_20260719.log
```

实际评估目录名以终端输出为准。确认下载文件可以打开后立即关机，AutoDL按实例开机时间
而不是GPU利用率计费。

## 10. 本地ONNX导出和INT8量化

本机已有Python 3.10和3.11。推荐使用Python 3.11创建独立环境，不污染ESP-IDF或
日常Python环境：

```powershell
cd D:\ESP32-IDF\esp32s3-cam\yolov11
py -3.11 -m venv .venv-espdl
.\.venv-espdl\Scripts\Activate.ps1
python -m pip install --upgrade pip
python -m pip install -r requirements-local-export.txt
```

将云端下载的最佳权重放到：

```text
D:\ESP32-IDF\esp32s3-cam\yolov11\best_cloud_20260719.pt
```

第一步，导出包含6个ESP-DL检测头输出的ONNX：

```powershell
python train_and_export.py export-onnx `
  --weights best_cloud_20260719.pt `
  --output solar_panel_yolo11n_cloud_20260719.onnx
```

输出：

```text
solar_panel_yolo11n_cloud_20260719.onnx
```

第二步，在CPU上用验证清单中的200张图片进行INT8校准：

```powershell
python train_and_export.py quantize `
  --onnx solar_panel_yolo11n_cloud_20260719.onnx `
  --output solar_panel_yolo11n_cloud_20260719.espdl
```

输出：

```text
solar_panel_yolo11n_cloud_20260719.espdl
```

量化不需要CUDA。16GB内存足够处理YOLO11n、320x320、batch 1的校准任务，但速度会
慢于GPU训练，这是正常现象。

默认关闭逐层误差报告，因为它在CPU上远慢于模型导出。需要单独诊断量化层误差时，
可以用较少图片执行：

```powershell
python train_and_export.py quantize --calibration-images 20 --error-report
```

诊断命令生成的模型不能替代使用200张校准图片生成的正式模型。

第三步，将模型复制进固件目录：

```powershell
python train_and_export.py deploy `
  --model solar_panel_yolo11n_cloud_20260719.espdl
```

目标文件：

```text
main/model/yolo11n_esp32s3.espdl
```

## 11. 固件验证

加载ESP-IDF 5.4.2环境后执行：

```powershell
idf.py build
idf.py flash monitor
```

量化步骤保留 `export_test_values=True`，板端 `model->test()` 可检查ESP-DL模型中的
测试输入输出。通过自检不等于检测效果已经达标，还要用OV5640分别拍摄六类已知场景，
核对Python输出ID、ESP32显示名称、LED状态和清洁决策。

## 12. 今晚的停止条件

今晚优先完成以下可验证目标：

1. RTX 5090被PyTorch正确识别，无 `sm_120` 警告。
2. 一轮冒烟测试成功。
3. 120轮上限的正式训练完成或正常早停。
4. 下载完整训练目录和日志。
5. 获得一次独立测试集指标。
6. 若时间允许，再在本地完成ONNX导出和INT8量化。

如果训练指标仍低，下一步应根据混淆矩阵和逐类指标修正数据，不应直接把轮数继续增加到
几百轮。

## 13. 2026-07-20实际发布结果

云端正式训练运行120轮，最终选择验证集综合指标略优的 `best.pt`。独立测试集结果：

```text
Precision    0.611435
Recall       0.399792
mAP50        0.413506
mAP50-95     0.201516
```

本地使用200张验证图片完成INT8校准，最终模型文件为
`solar_panel_yolo11n_cloud_20260719.espdl`。模型包含标量 `RequantizeLinear` 参数，
ESP-DL 3.3.3无法正确加载；项目因此固定使用ESP-DL 3.3.5，这是首个包含官方
`Support scalar export`修复的稳定版本。

ESP32-S3板端验证结果：

```text
Model self-test PASSED
输入：1 x 320 x 320 x 3
输出：box0/score0、box1/score1、box2/score2
连续推理：正常，无崩溃或重启
单帧总耗时：约4.45秒
```

发布模型SHA256：

```text
best_cloud_20260719.pt
7252B196350F92696B6F94A59B5F50EF888FFEAD480FA48A5F356110157BE116

solar_panel_yolo11n_cloud_20260719.onnx
404B86B9BAB31C7948DFF23C940B0BE523F00CC1B68A364456CC6ABAF8227379

solar_panel_yolo11n_cloud_20260719.espdl
8F31A2097B9E4D5C7E1CCCFD94597C89EE0441D2589003E70FB68CDD73ABB88C
```
