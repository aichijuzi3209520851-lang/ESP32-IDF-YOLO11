"""
YOLO11n 太阳能板缺陷检测 — 训练 → ESP-DL导出 → INT8量化 一键脚本

用法:  python train_and_export.py

输出:  solar_panel_yolo11n.espdl  (拷贝到 ../main/model/ 即可部署)

历史经验:
  - Windows 必须用 if __name__ == "__main__" 包裹 (multiprocessing spawn)
  - ESP-DL 需要分离 box/score 输出 (ESP_Detect), DFL 解码移至后处理
  - 输入校准用 [0,1] 归一化; C++ 推理时需同步做 /255.0
  - INT8 score_scale=8 时最小可表达 sigmoid=0.5, 阈值用 -1 检测 int8≥0
  - export_test_values=True 才能启用板端 model->test()
"""

import os, shutil
import numpy as np
import torch
from torch.utils.data import DataLoader, Dataset
from PIL import Image

# ============================================================
# 配置
# ============================================================
IMG_SIZE = 320            # 模型输入分辨率 (与摄像头一致)
MODEL_NAME = "yolo11n"    # 骨架: yolo11n / yolo11s / yolo11m
NC = 6                    # 检测类别数
CLASS_NAMES = ['bird', 'crack', 'dust', 'shadow', 'hotspot', 'panel']

ONNX_NAME  = "solar_panel_yolo11n.onnx"
ESPDL_NAME = "solar_panel_yolo11n.espdl"
BEST_PT    = "runs/detect/train/weights/best.pt"

# 训练超参数 (V6: 降误判优先)
TRAIN_CFG = dict(
    data="data.yaml", epochs=30, imgsz=IMG_SIZE,
    batch=4, device="cpu", workers=0, patience=15,
    save=True, save_period=10, project="runs", name="train", exist_ok=True,
    pretrained=True, optimizer="AdamW", cos_lr=True, amp=False, cache=True,
    # 学习率
    lr0=0.001, lrf=0.01, warmup_epochs=3,
    # 数据增强
    augment=True,
    hsv_h=0.01, hsv_s=0.40, hsv_v=0.20,
    degrees=0.3, translate=0.02, scale=0.06, shear=0.10,
    fliplr=0.5, flipud=0.0, perspective=0.0,
    mosaic=0.15, mixup=0.03, copy_paste=0.35, erasing=0.10,
    close_mosaic=10, multi_scale=False, rect=False,
    # 损失权重
    box=7.5, cls=1.25, dfl=1.2, dropout=0.18,
)


# ============================================================
# 校准数据集 (用于 ESP-PPQ 量化)
# ============================================================
class CalibDataset(Dataset):
    def __init__(self, img_dir, img_size=IMG_SIZE):
        self.img_dir = img_dir
        self.img_size = img_size
        self.files = sorted([
            f for f in os.listdir(img_dir)
            if f.lower().endswith((".jpg", ".jpeg", ".png", ".bmp"))
        ])

    def __len__(self):
        return len(self.files)

    def __getitem__(self, idx):
        img = Image.open(os.path.join(self.img_dir, self.files[idx])).convert("RGB")
        img = img.resize((self.img_size, self.img_size), Image.BILINEAR)
        arr = np.asarray(img, dtype=np.float32) / 255.0       # [0,1] 归一化
        return torch.from_numpy(arr).permute(2, 0, 1).contiguous()  # CHW


# ============================================================
# 主流程
# ============================================================
def main():
    os.chdir(os.path.dirname(os.path.abspath(__file__)))

    # ===== STEP 1: 训练 =====
    print("=" * 60)
    print(f"STEP 1: 训练 {MODEL_NAME} ({IMG_SIZE}x{IMG_SIZE}, CUDA)")
    print("=" * 60)

    from ultralytics import YOLO

    model = YOLO(f"{MODEL_NAME}.pt")   # 自动下载预训练权重
    results = model.train(**TRAIN_CFG)
    model.val()

    best = str(results.save_dir / "weights/best.pt")
    print(f"Best model: {best}")

    # ===== STEP 2: 导出 ESP-DL 兼容 ONNX =====
    print("\n" + "=" * 60)
    print("STEP 2: 导出 ESP-DL 兼容 ONNX")
    print("=" * 60)

    import onnx
    from ultralytics.nn.modules import Detect, Attention
    from ultralytics.engine.exporter import Exporter, try_export, arange_patch
    from ultralytics.utils import LOGGER, colorstr
    from ultralytics.utils.checks import check_requirements

    # ---- ESP-DL 自定义检测头: 分离 box/score, 移除 DFL ----
    class ESP_Detect(Detect):
        def forward(self, x):
            return (
                self.cv2[0](x[0]), self.cv3[0](x[0]),  # stride 8
                self.cv2[1](x[1]), self.cv3[1](x[1]),  # stride 16
                self.cv2[2](x[2]), self.cv3[2](x[2]),  # stride 32
            )

    class ESP_Attention(Attention):
        def forward(self, x):
            B, C, H, W = x.shape
            N = H * W
            qkv = self.qkv(x)
            q, k, v = qkv.view(-1, self.num_heads, self.key_dim * 2 + self.head_dim, N) \
                       .split([self.key_dim, self.key_dim, self.head_dim], dim=2)
            attn = (q.transpose(-2, -1) @ k) * self.scale
            attn = attn.softmax(dim=-1)
            x = (v @ attn.transpose(-2, -1)).view(-1, C, H, W) + self.pe(v.reshape(-1, C, H, W))
            return self.proj(x)

    class ESP_Detect_Exporter(Exporter):
        @try_export
        def export_onnx(self, prefix=colorstr("ONNX:")):
            check_requirements(["onnx>=1.14.0"])
            f = ONNX_NAME
            output_names = ["box0", "score0", "box1", "score1", "box2", "score2"]

            with arange_patch(self.args):
                torch.onnx.export(self.model, self.im, f,
                    verbose=False, opset_version=13,
                    do_constant_folding=False,
                    input_names=["images"], output_names=output_names)

            model_onnx = onnx.load(f)
            onnx.save(model_onnx, f)
            LOGGER.info(f"{prefix} saved: {f}")
            return f, model_onnx

    # Patch + export
    model2 = YOLO(best)
    for m in model2.modules():
        if isinstance(m, Attention): m.forward = ESP_Attention.forward.__get__(m)
        if isinstance(m, Detect):    m.forward = ESP_Detect.forward.__get__(m)

    custom = {"format": "onnx", "imgsz": IMG_SIZE, "batch": 1, "data": None,
              "device": None, "verbose": False}
    args = {**model2.overrides, **custom, "mode": "export"}
    ESP_Detect_Exporter(overrides=args, _callbacks=model2.callbacks)(model=model2.model)
    print(f"ONNX 导出完成: {ONNX_NAME}")

    # ===== STEP 3: INT8 量化 (ESP-PPQ → .espdl) =====
    print("\n" + "=" * 60)
    print("STEP 3: ESP-PPQ INT8 量化 → .espdl")
    print("=" * 60)

    calib_dir = "val/images"
    if not os.path.isdir(calib_dir):
        print(f"WARNING: 验证集 {calib_dir} 不存在, 使用 train/images")
        calib_dir = "train/images"

    dataset = CalibDataset(calib_dir)
    n_calib = min(200, len(dataset))
    indices = list(range(0, len(dataset), max(1, len(dataset) // n_calib)))[:n_calib]
    calib_subset = torch.utils.data.Subset(dataset, indices)
    calib_loader = DataLoader(calib_subset, batch_size=1, shuffle=False, num_workers=0)
    print(f"校准样本: {len(calib_subset)} 张")

    from esp_ppq.api import espdl_quantize_onnx

    espdl_quantize_onnx(
        onnx_import_file=ONNX_NAME,
        espdl_export_file=ESPDL_NAME,
        calib_dataloader=calib_loader,
        calib_steps=len(calib_subset),
        input_shape=[1, 3, IMG_SIZE, IMG_SIZE],
        target="esp32s3",
        num_of_bits=8,
        device="cpu",
        error_report=True,
        verbose=1,
        export_test_values=True,        # 启用板端 model->test()
        test_output_names=[
            "box0", "score0", "box1", "score1", "box2", "score2",
        ],
    )

    sz = os.path.getsize(ESPDL_NAME) / (1024 * 1024)
    print(f"\nESP-DL 模型: {ESPDL_NAME} ({sz:.2f} MB)")

    # 自动部署到固件目录
    dst = "../main/model/yolo11n_esp32s3.espdl"
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy(ESPDL_NAME, dst)
    print(f"已复制到 {dst}")
    print("\n完成! 运行 'idf.py build flash monitor' 部署")


if __name__ == "__main__":
    main()
