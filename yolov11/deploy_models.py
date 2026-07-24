"""
训练后部署脚本
==============
云端训练完成后，在本地执行以下操作：

ESP32 模型部署:
  python deploy_models.py export-esp32 --weights cloud_results_20260724/esp32/weights/best.pt
  python deploy_models.py quantize-esp32
  python deploy_models.py deploy-esp32

PC 模型部署:
  python deploy_models.py export-pc --weights cloud_results_20260724/pc/weights/best.pt
  (PC 模型不需要量化，直接使用 FP32 ONNX)
"""

import argparse
import os
import shutil
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
FIRMWARE_MODEL_DIR = SCRIPT_DIR.parent / "main" / "model"

# ESP32 模型配置
ESP32_IMG_SIZE = 320
ESP32_CLASS_NAMES = ["stain", "damage"]
ESP32_NC = 2

# PC 模型配置
PC_IMG_SIZE = 640
PC_CLASS_NAMES = ["bird_drop", "dust", "electrical_damage", "physical_damage", "snow_covered"]
PC_NC = 5


def resolve_path(path: str) -> Path:
    candidate = Path(path).expanduser()
    if not candidate.is_absolute():
        candidate = SCRIPT_DIR / candidate
    return candidate.resolve()


def require_file(path: Path, label: str) -> Path:
    if not path.is_file():
        raise FileNotFoundError(f"{label} not found: {path}")
    return path


def export_esp32_onnx(args: argparse.Namespace) -> None:
    """将 ESP32 的 best.pt 导出为 ESP-DL 兼容的 ONNX"""
    import onnx
    import torch
    from ultralytics import YOLO
    from ultralytics.engine.exporter import Exporter, arange_patch, try_export
    from ultralytics.nn.modules import Attention, Detect
    from ultralytics.utils import LOGGER, colorstr
    from ultralytics.utils.checks import check_requirements

    weights = require_file(resolve_path(args.weights), "Weights")
    output = resolve_path(args.output)

    print(f"Exporting ESP32 ONNX...")
    print(f"  Weights: {weights}")
    print(f"  Output:  {output}")
    print(f"  Size:    {ESP32_IMG_SIZE}×{ESP32_IMG_SIZE}")
    print(f"  Classes: {ESP32_CLASS_NAMES} ({ESP32_NC})")

    # ESP-DL 兼容的 Detect 和 Attention 头
    class ESPDetect(Detect):
        def forward(self, x):
            return (
                self.cv2[0](x[0]), self.cv3[0](x[0]),
                self.cv2[1](x[1]), self.cv3[1](x[1]),
                self.cv2[2](x[2]), self.cv3[2](x[2]),
            )

    class ESPAttention(Attention):
        def forward(self, x):
            batch, channels, height, width = x.shape
            count = height * width
            qkv = self.qkv(x)
            q, k, v = qkv.view(
                -1, self.num_heads, self.key_dim * 2 + self.head_dim, count
            ).split([self.key_dim, self.key_dim, self.head_dim], dim=2)
            attention = (q.transpose(-2, -1) @ k) * self.scale
            attention = attention.softmax(dim=-1)
            x = (v @ attention.transpose(-2, -1)).view(
                -1, channels, height, width
            ) + self.pe(v.reshape(-1, channels, height, width))
            return self.proj(x)

    class ESPDetectExporter(Exporter):
        @try_export
        def export_onnx(self, prefix=colorstr("ONNX:")):
            check_requirements(["onnx>=1.14.0,<1.18.0"])
            output_names = ["box0", "score0", "box1", "score1", "box2", "score2"]

            with arange_patch(self.args):
                torch.onnx.export(
                    self.model,
                    self.im,
                    str(output),
                    verbose=False,
                    opset_version=13,
                    do_constant_folding=False,
                    input_names=["images"],
                    output_names=output_names,
                )

            model_onnx = onnx.load(str(output))
            onnx.checker.check_model(model_onnx)
            onnx.save(model_onnx, str(output))
            LOGGER.info(f"{prefix} saved: {output}")
            return str(output)

    model = YOLO(str(weights))
    for module in model.modules():
        if isinstance(module, Attention):
            module.forward = ESPAttention.forward.__get__(module)
        if isinstance(module, Detect):
            module.forward = ESPDetect.forward.__get__(module)

    custom = {
        "format": "onnx",
        "imgsz": ESP32_IMG_SIZE,
        "batch": 1,
        "data": None,
        "device": "cpu",
        "verbose": False,
    }
    exporter_args = {**model.overrides, **custom, "mode": "export"}
    ESPDetectExporter(overrides=exporter_args, _callbacks=model.callbacks)(model=model.model)

    size_mb = output.stat().st_size / (1024 * 1024)
    print(f"\nONNX export complete: {output} ({size_mb:.2f} MB)")


def quantize_esp32(args: argparse.Namespace) -> None:
    """对 ESP32 ONNX 模型进行 INT8 量化"""
    import numpy as np
    import torch
    from PIL import Image
    from torch.utils.data import DataLoader, Dataset, Subset
    from esp_ppq.api import espdl_quantize_onnx

    onnx_path = require_file(resolve_path(args.onnx), "ONNX model")
    manifest = resolve_path(args.calibration)
    if not manifest.exists():
        raise FileNotFoundError(f"Calibration manifest not found: {manifest}")
    output = resolve_path(args.output)

    print(f"Quantizing ESP32 model...")
    print(f"  ONNX:        {onnx_path}")
    print(f"  Calibration: {manifest} ({args.calibration_images} images)")
    print(f"  Output:      {output}")

    # 构建校准数据加载器
    class CalibrationDataset(Dataset):
        def __init__(self, list_path: Path):
            lines = [line.strip() for line in list_path.read_text("utf-8").splitlines()]
            self.files = []
            for line in lines:
                if line and not line.startswith("#"):
                    # 校准清单中的路径是相对于 datasets/esp32_2class/ 的
                    img_path = SCRIPT_DIR / "datasets" / "esp32_2class" / line
                    if img_path.is_file():
                        self.files.append(img_path)
            if not self.files:
                raise ValueError(f"No calibration images found in: {list_path}")
            print(f"  Found {len(self.files)} calibration images")

        def __len__(self):
            return len(self.files)

        def __getitem__(self, index):
            image = Image.open(self.files[index]).convert("RGB")
            image = image.resize((ESP32_IMG_SIZE, ESP32_IMG_SIZE), Image.Resampling.BILINEAR)
            array = np.asarray(image, dtype=np.float32) / 255.0
            return torch.from_numpy(array).permute(2, 0, 1).contiguous()

    dataset = CalibrationDataset(manifest)
    sample_count = min(args.calibration_images, len(dataset))
    indices = [index * len(dataset) // sample_count for index in range(sample_count)]
    subset = Subset(dataset, indices)
    loader = DataLoader(subset, batch_size=1, shuffle=False, num_workers=0)

    espdl_quantize_onnx(
        onnx_import_file=str(onnx_path),
        espdl_export_file=str(output),
        calib_dataloader=loader,
        calib_steps=sample_count,
        input_shape=[1, 3, ESP32_IMG_SIZE, ESP32_IMG_SIZE],
        target="esp32s3",
        num_of_bits=8,
        device="cpu",
        error_report=args.error_report,
        verbose=1,
        export_test_values=True,
        test_output_names=["box0", "score0", "box1", "score1", "box2", "score2"],
    )

    size_mb = output.stat().st_size / (1024 * 1024)
    print(f"\nQuantization complete: {output} ({size_mb:.2f} MB)")


def deploy_esp32(args: argparse.Namespace) -> None:
    """将 ESP-DL 模型复制到固件目录"""
    source = require_file(resolve_path(args.model), "ESP-DL model")
    destination = resolve_path(args.destination)
    destination.parent.mkdir(parents=True, exist_ok=True)

    # 备份旧模型
    if destination.exists():
        backup = destination.with_suffix(".espdl.bak")
        print(f"Backing up old model to: {backup}")
        shutil.copy2(destination, backup)

    shutil.copy2(source, destination)
    size_mb = destination.stat().st_size / (1024 * 1024)
    print(f"Model deployed: {destination} ({size_mb:.2f} MB)")
    print(f"\n  现在可以编译固件了:")
    print(f"  cd {SCRIPT_DIR.parent}")
    print(f"  idf.py build")


def export_pc_onnx(args: argparse.Namespace) -> None:
    """导出 PC 模型的 FP32 ONNX（不需要量化）"""
    import torch
    from ultralytics import YOLO

    weights = require_file(resolve_path(args.weights), "Weights")
    output = resolve_path(args.output)

    print(f"Exporting PC ONNX (FP32, no quantization)...")
    print(f"  Weights: {weights}")
    print(f"  Output:  {output}")
    print(f"  Size:    {PC_IMG_SIZE}×{PC_IMG_SIZE}")
    print(f"  Classes: {PC_CLASS_NAMES} ({PC_NC})")

    model = YOLO(str(weights))
    model.export(
        format="onnx",
        imgsz=PC_IMG_SIZE,
        simplify=True,
        opset=13,
        device="cpu",
    )

    # 重命名到指定路径
    default_onnx = weights.with_suffix(".onnx")
    if default_onnx.exists() and default_onnx != output:
        shutil.move(str(default_onnx), str(output))

    size_mb = output.stat().st_size / (1024 * 1024)
    print(f"\nONNX export complete: {output} ({size_mb:.2f} MB)")
    print(f"\n  PC 模型 ONNX 已导出，后续在 PC 端使用:")
    print(f"  D:\\ESP32-IDF\\YOLO11\\models\\pc_model.onnx")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="训练后模型部署")
    subparsers = parser.add_subparsers(dest="command", required=True)

    # --- ESP32: export ONNX ---
    export_esp32 = subparsers.add_parser("export-esp32", help="导出 ESP32 模型 ONNX")
    export_esp32.add_argument("--weights", required=True, help="best.pt 路径")
    export_esp32.add_argument(
        "--output",
        default=str(SCRIPT_DIR / "esp32_2class_model.onnx"),
    )
    export_esp32.set_defaults(handler=export_esp32_onnx)

    # --- ESP32: quantize ---
    quantize_esp32_parser = subparsers.add_parser("quantize-esp32", help="INT8 量化 ESP32 模型")
    quantize_esp32_parser.add_argument(
        "--onnx",
        default=str(SCRIPT_DIR / "esp32_2class_model.onnx"),
    )
    quantize_esp32_parser.add_argument(
        "--output",
        default=str(SCRIPT_DIR / "esp32_2class_model.espdl"),
    )
    quantize_esp32_parser.add_argument(
        "--calibration",
        default="datasets/esp32_2class/splits/val.txt",
    )
    quantize_esp32_parser.add_argument("--calibration-images", type=int, default=200)
    quantize_esp32_parser.add_argument("--error-report", action="store_true")
    quantize_esp32_parser.set_defaults(handler=quantize_esp32)

    # --- ESP32: deploy ---
    deploy_esp32_parser = subparsers.add_parser("deploy-esp32", help="部署模型到固件")
    deploy_esp32_parser.add_argument(
        "--model",
        default=str(SCRIPT_DIR / "esp32_2class_model.espdl"),
    )
    deploy_esp32_parser.add_argument(
        "--destination",
        default=str(FIRMWARE_MODEL_DIR / "yolo11n_esp32s3.espdl"),
    )
    deploy_esp32_parser.set_defaults(handler=deploy_esp32)

    # --- PC: export ONNX ---
    export_pc = subparsers.add_parser("export-pc", help="导出 PC 模型 ONNX")
    export_pc.add_argument("--weights", required=True, help="best.pt 路径")
    export_pc.add_argument(
        "--output",
        default=str(SCRIPT_DIR / "pc_5class_model.onnx"),
    )
    export_pc.set_defaults(handler=export_pc_onnx)

    return parser


def main() -> None:
    os.chdir(SCRIPT_DIR)
    args = build_parser().parse_args()
    args.handler(args)


if __name__ == "__main__":
    main()