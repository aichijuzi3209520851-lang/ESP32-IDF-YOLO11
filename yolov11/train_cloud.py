"""
云端训练脚本 (在 AutoDL 上运行)
===============================
支持两种模型训练:

  ESP32 轻量模型 (YOLO11n, 320×320, 2类):
    python train_cloud.py esp32

  PC 高精度模型 (YOLO11s, 640×640, 5类):
    python train_cloud.py pc

  python train_cloud.py esp32 --resume   # 断点续训
"""

import argparse
import json
import os
import sys
from datetime import datetime
from pathlib import Path

# AutoDL 典型挂载路径
AUTODL_BASE = Path("/root/autodl-tmp")

# ============================================================
# 模型配置
# ============================================================

MODEL_CONFIGS = {
    "esp32": {
        "description": "ESP32轻量模型 - 2类 (stain, damage)",
        "model": "yolo11n.pt",
        "data": "datasets/esp32_2class/data.yaml",
        "imgsz": 320,
        "epochs": 150,
        "batch": 64,
        "nc": 2,
        "names": ["stain", "damage"],
        "output_dir": "runs/esp32",
    },
    "pc": {
        "description": "PC高精度模型 - 5类 (bird_drop, dust, electrical_damage, physical_damage, snow_covered)",
        "model": "yolo11s.pt",
        "data": "datasets/pc_5class/data.yaml",
        "imgsz": 640,
        "epochs": 150,
        "batch": 32,
        "nc": 5,
        "names": ["bird_drop", "dust", "electrical_damage", "physical_damage", "snow_covered"],
        "output_dir": "runs/pc",
    },
}


def check_environment():
    """检查 GPU 环境和依赖"""
    import torch

    print("=" * 60)
    print("  环境检查")
    print("=" * 60)
    print(f"  Python:    {sys.version.split()[0]}")
    print(f"  PyTorch:   {torch.__version__}")
    print(f"  CUDA:      {torch.version.cuda}")
    print(f"  GPU:       {torch.cuda.get_device_name(0) if torch.cuda.is_available() else 'NOT AVAILABLE'}")
    print(f"  GPU Count: {torch.cuda.device_count()}")

    if not torch.cuda.is_available():
        print("\n  *** 错误: CUDA 不可用！请检查 AutoDL 镜像 ***")
        sys.exit(1)

    # 检查数据集
    for model_type in ["esp32", "pc"]:
        cfg = MODEL_CONFIGS[model_type]
        data_path = AUTODL_BASE / "yolov11" / cfg["data"]
        if data_path.exists():
            print(f"  Dataset [{model_type}]: OK ({data_path})")
        else:
            print(f"  Dataset [{model_type}]: MISSING ({data_path})")


def train_model(model_type: str, resume: bool = False):
    """训练模型"""
    import torch
    from ultralytics import YOLO

    cfg = MODEL_CONFIGS[model_type]
    print(f"\n{'='*60}")
    print(f"  开始训练: {cfg['description']}")
    print(f"{'='*60}")
    print(f"  模型:     {cfg['model']}")
    print(f"  输入尺寸: {cfg['imgsz']}×{cfg['imgsz']}")
    print(f"  类别数:   {cfg['nc']} ({', '.join(cfg['names'])})")
    print(f"  轮数:     {cfg['epochs']}")
    print(f"  Batch:    {cfg['batch']}")
    print(f"  时间:      {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")

    data_path = AUTODL_BASE / "yolov11" / cfg["data"]
    if not data_path.exists():
        print(f"\n  *** 错误: 数据集不存在: {data_path} ***")
        print(f"  请确认已正确解压训练包到 {AUTODL_BASE / 'yolov11'}")
        sys.exit(1)

    model = YOLO(cfg["model"])

    if resume:
        # 断点续训：查找 last.pt
        last_pt = AUTODL_BASE / "yolov11" / cfg["output_dir"] / "weights" / "last.pt"
        if not last_pt.exists():
            print(f"\n  *** 错误: 找不到断点文件 {last_pt} ***")
            sys.exit(1)
        print(f"\n  从断点恢复: {last_pt}")
        model = YOLO(str(last_pt))
        results = model.train(resume=True)
    else:
        results = model.train(
            data=str(data_path),
            epochs=cfg["epochs"],
            imgsz=cfg["imgsz"],
            batch=cfg["batch"],
            device=0,
            workers=4,
            patience=30,
            save=True,
            save_period=10,
            project=str(AUTODL_BASE / "yolov11" / "runs"),
            name=model_type,
            exist_ok=False,
            pretrained=True,
            optimizer="AdamW",
            cos_lr=True,
            amp=False,
            cache="disk",
            lr0=0.001,
            lrf=0.01,
            warmup_epochs=3,
            augment=True,
            hsv_h=0.01,
            hsv_s=0.40,
            hsv_v=0.20,
            degrees=0.3,
            translate=0.02,
            scale=0.06,
            shear=0.10,
            fliplr=0.5,
            flipud=0.0,
            perspective=0.0,
            mosaic=0.15,
            mixup=0.03,
            copy_paste=0.35,
            erasing=0.10,
            close_mosaic=10,
            multi_scale=False,
            rect=False,
            box=7.5,
            cls=1.25,
            dfl=1.2,
            dropout=0.18,
            plots=True,
        )

    # 输出结果
    best = Path(results.save_dir) / "weights" / "best.pt"
    last = Path(results.save_dir) / "weights" / "last.pt"
    print(f"\n{'='*60}")
    print(f"  训练完成！")
    print(f"{'='*60}")
    print(f"  输出目录:  {results.save_dir}")
    print(f"  最佳权重:  {best}")
    print(f"  最后权重:  {last}")
    print(f"\n  *** 请截图保存以上训练结果信息 ***")
    print(f"  *** 关停 AutoDL 实例前，务必下载整个输出目录 ***")


def validate_model(model_type: str):
    """在测试集上评估模型"""
    import torch
    from ultralytics import YOLO

    cfg = MODEL_CONFIGS[model_type]
    best_pt = AUTODL_BASE / "yolov11" / cfg["output_dir"] / "weights" / "best.pt"

    if not best_pt.exists():
        print(f"ERROR: 找不到模型权重: {best_pt}")
        sys.exit(1)

    print(f"\n{'='*60}")
    print(f"  测试集评估: {cfg['description']}")
    print(f"{'='*60}")

    model = YOLO(str(best_pt))
    data_path = AUTODL_BASE / "yolov11" / cfg["data"]

    results = model.val(
        data=str(data_path),
        split="test",
        imgsz=cfg["imgsz"],
        batch=cfg["batch"],
        device=0,
        workers=4,
        project=str(AUTODL_BASE / "yolov11" / "runs"),
        name=f"{model_type}_test",
        exist_ok=True,
        plots=True,
    )

    print(f"\n{'='*60}")
    print(f"  测试集指标")
    print(f"{'='*60}")
    for name, value in results.results_dict.items():
        print(f"  {name}: {float(value):.6f}")

    if results.box is not None:
        print(f"\n  各类别 mAP50-95:")
        for class_id, value in enumerate(results.box.maps):
            print(f"    {class_id} {cfg['names'][class_id]}: {float(value):.6f}")

    print(f"\n  *** 请截图保存以上测试结果 ***")


def main():
    parser = argparse.ArgumentParser(description="云端训练 YOLO 模型")
    parser.add_argument(
        "model_type",
        choices=["esp32", "pc"],
        help="esp32: ESP32轻量模型, pc: PC高精度模型",
    )
    parser.add_argument(
        "--resume",
        action="store_true",
        help="从断点恢复训练",
    )
    parser.add_argument(
        "--validate-only",
        action="store_true",
        help="仅评估，不训练",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="仅检查环境，不训练",
    )
    args = parser.parse_args()

    os.chdir(AUTODL_BASE / "yolov11")

    if args.check:
        check_environment()
    elif args.validate_only:
        check_environment()
        validate_model(args.model_type)
    else:
        check_environment()
        train_model(args.model_type, resume=args.resume)
        validate_model(args.model_type)


if __name__ == "__main__":
    main()
