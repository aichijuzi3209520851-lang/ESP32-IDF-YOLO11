"""
YOLO11n solar-panel model pipeline.

Cloud training:
  python train_and_export.py train
  python train_and_export.py validate --weights runs/detect/train_cloud/weights/best.pt --split val
  python train_and_export.py validate --weights runs/detect/train_cloud/weights/best.pt --split test

Local conversion:
  python train_and_export.py export-onnx --weights best.pt
  python train_and_export.py quantize
  python train_and_export.py deploy
"""

import argparse
import os
import shutil
from pathlib import Path

import yaml


SCRIPT_DIR = Path(__file__).resolve().parent
DATA_CONFIG_PATH = SCRIPT_DIR / "data.yaml"
DEFAULT_ONNX = SCRIPT_DIR / "solar_panel_yolo11n.onnx"
DEFAULT_ESPDL = SCRIPT_DIR / "solar_panel_yolo11n.espdl"
DEFAULT_DEPLOY_PATH = SCRIPT_DIR.parent / "main" / "model" / "yolo11n_esp32s3.espdl"
IMG_SIZE = 320

with DATA_CONFIG_PATH.open("r", encoding="utf-8") as file:
    DATA_CONFIG = yaml.safe_load(file)

CLASS_NAMES = [DATA_CONFIG["names"][index] for index in sorted(DATA_CONFIG["names"])]
NC = len(CLASS_NAMES)

# Competition training preset: prioritize fewer false positives while preserving
# the 320x320 input required by the ESP32-S3 deployment.
TRAIN_CFG = dict(
    data=str(DATA_CONFIG_PATH),
    epochs=100,
    imgsz=IMG_SIZE,
    batch=32,
    device=0,
    workers=4,
    patience=20,
    save=True,
    save_period=10,
    project=str(SCRIPT_DIR / "runs" / "detect"),
    name="train_cloud",
    exist_ok=False,
    pretrained=True,
    optimizer="AdamW",
    cos_lr=True,
    amp=True,
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
)


def resolve_path(path: str) -> Path:
    candidate = Path(path).expanduser()
    if not candidate.is_absolute():
        candidate = SCRIPT_DIR / candidate
    return candidate.resolve()


def require_file(path: Path, label: str) -> Path:
    if not path.is_file():
        raise FileNotFoundError(f"{label} not found: {path}")
    return path


def train(args: argparse.Namespace) -> None:
    import torch
    from ultralytics import YOLO

    if str(args.device).lower() != "cpu" and not torch.cuda.is_available():
        raise RuntimeError("CUDA is unavailable. Check the AutoDL image before paid training.")

    print(f"PyTorch: {torch.__version__}")
    print(f"CUDA runtime: {torch.version.cuda}")
    if torch.cuda.is_available():
        print(f"GPU: {torch.cuda.get_device_name(0)}")

    model_path = resolve_path(args.model)
    model = YOLO(str(model_path) if model_path.is_file() else args.model)
    if args.resume:
        if not model_path.is_file():
            raise FileNotFoundError("--resume requires --model to point to last.pt")
        results = model.train(resume=True)
    else:
        config = dict(TRAIN_CFG)
        config.update(
            epochs=args.epochs,
            batch=args.batch,
            device=args.device,
            workers=args.workers,
            patience=args.patience,
            name=args.name,
        )
        results = model.train(**config)
    best = results.save_dir / "weights" / "best.pt"
    last = results.save_dir / "weights" / "last.pt"

    print(f"\nTraining output: {results.save_dir}")
    print(f"Best weights: {best}")
    print(f"Last weights: {last}")
    print("Download the complete training output directory before shutting down AutoDL.")


def validate(args: argparse.Namespace) -> None:
    import torch
    from ultralytics import YOLO

    weights = require_file(resolve_path(args.weights), "Weights")
    if str(args.device).lower() != "cpu" and not torch.cuda.is_available():
        raise RuntimeError("CUDA is unavailable. Use --device cpu or fix the GPU environment.")

    model = YOLO(str(weights))
    results = model.val(
        data=str(DATA_CONFIG_PATH),
        split=args.split,
        imgsz=IMG_SIZE,
        batch=args.batch,
        device=args.device,
        workers=args.workers,
        project=str(SCRIPT_DIR / "runs" / "detect"),
        name=f"{args.split}_{weights.stem}",
        exist_ok=True,
        plots=True,
    )

    print(f"\n{args.split} metrics:")
    for name, value in results.results_dict.items():
        print(f"  {name}: {float(value):.6f}")
    if results.box is not None:
        print("Per-class mAP50-95:")
        for class_id, value in enumerate(results.box.maps):
            print(f"  {class_id} {CLASS_NAMES[class_id]}: {float(value):.6f}")
    print(f"Validation output: {results.save_dir}")


def export_onnx(args: argparse.Namespace) -> None:
    import onnx
    import torch
    from ultralytics import YOLO
    from ultralytics.engine.exporter import Exporter, arange_patch, try_export
    from ultralytics.nn.modules import Attention, Detect
    from ultralytics.utils import LOGGER, colorstr
    from ultralytics.utils.checks import check_requirements

    weights = require_file(resolve_path(args.weights), "Weights")
    output = resolve_path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)

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
        "imgsz": IMG_SIZE,
        "batch": 1,
        "data": None,
        "device": "cpu",
        "verbose": False,
    }
    exporter_args = {**model.overrides, **custom, "mode": "export"}
    ESPDetectExporter(overrides=exporter_args, _callbacks=model.callbacks)(model=model.model)
    print(f"ONNX export complete: {output}")


def build_calibration_loader(manifest: Path, image_count: int):
    import numpy as np
    import torch
    from PIL import Image
    from torch.utils.data import DataLoader, Dataset, Subset

    class CalibrationDataset(Dataset):
        def __init__(self, list_path: Path):
            lines = [line.strip() for line in list_path.read_text(encoding="utf-8").splitlines()]
            self.files = [
                (SCRIPT_DIR / line).resolve()
                for line in lines
                if line and not line.startswith("#")
            ]
            missing = [path for path in self.files if not path.is_file()]
            if missing:
                raise FileNotFoundError(f"Calibration image not found: {missing[0]}")
            if not self.files:
                raise ValueError(f"Calibration manifest is empty: {list_path}")

        def __len__(self):
            return len(self.files)

        def __getitem__(self, index):
            image = Image.open(self.files[index]).convert("RGB")
            image = image.resize((IMG_SIZE, IMG_SIZE), Image.Resampling.BILINEAR)
            array = np.asarray(image, dtype=np.float32) / 255.0
            return torch.from_numpy(array).permute(2, 0, 1).contiguous()

    dataset = CalibrationDataset(manifest)
    sample_count = min(image_count, len(dataset))
    indices = [index * len(dataset) // sample_count for index in range(sample_count)]
    subset = Subset(dataset, indices)
    loader = DataLoader(subset, batch_size=1, shuffle=False, num_workers=0)
    return loader, sample_count


def quantize(args: argparse.Namespace) -> None:
    from esp_ppq.api import espdl_quantize_onnx

    onnx_path = require_file(resolve_path(args.onnx), "ONNX model")
    manifest = require_file(resolve_path(args.calibration), "Calibration manifest")
    output = resolve_path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    loader, sample_count = build_calibration_loader(manifest, args.calibration_images)

    print(f"Calibration images: {sample_count}")
    espdl_quantize_onnx(
        onnx_import_file=str(onnx_path),
        espdl_export_file=str(output),
        calib_dataloader=loader,
        calib_steps=sample_count,
        input_shape=[1, 3, IMG_SIZE, IMG_SIZE],
        target="esp32s3",
        num_of_bits=8,
        device="cpu",
        error_report=args.error_report,
        verbose=1,
        export_test_values=True,
        test_output_names=["box0", "score0", "box1", "score1", "box2", "score2"],
    )

    size_mb = output.stat().st_size / (1024 * 1024)
    print(f"ESP-DL quantization complete: {output} ({size_mb:.2f} MB)")


def deploy(args: argparse.Namespace) -> None:
    source = require_file(resolve_path(args.model), "ESP-DL model")
    destination = resolve_path(args.destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    print(f"Model deployed: {destination}")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run cloud training and local ESP-DL conversion as explicit steps."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    train_parser = subparsers.add_parser("train", help="Train YOLO11n (AutoDL GPU).")
    train_parser.add_argument("--model", default="yolo11n.pt")
    train_parser.add_argument("--epochs", type=int, default=100)
    train_parser.add_argument("--batch", type=int, default=32)
    train_parser.add_argument("--device", default="0")
    train_parser.add_argument("--workers", type=int, default=4)
    train_parser.add_argument("--patience", type=int, default=20)
    train_parser.add_argument("--name", default="train_cloud")
    train_parser.add_argument(
        "--resume",
        action="store_true",
        help="Resume an interrupted run; --model must point to its last.pt.",
    )
    train_parser.set_defaults(handler=train)

    validate_parser = subparsers.add_parser("validate", help="Evaluate weights on val or test.")
    validate_parser.add_argument("--weights", required=True)
    validate_parser.add_argument("--split", choices=("val", "test"), default="val")
    validate_parser.add_argument("--batch", type=int, default=32)
    validate_parser.add_argument("--device", default="0")
    validate_parser.add_argument("--workers", type=int, default=4)
    validate_parser.set_defaults(handler=validate)

    export_parser = subparsers.add_parser(
        "export-onnx", help="Export ESP-DL-compatible ONNX on the local computer."
    )
    export_parser.add_argument("--weights", required=True)
    export_parser.add_argument("--output", default=str(DEFAULT_ONNX))
    export_parser.set_defaults(handler=export_onnx)

    quantize_parser = subparsers.add_parser(
        "quantize", help="Quantize ONNX to INT8 ESP-DL on the local CPU."
    )
    quantize_parser.add_argument("--onnx", default=str(DEFAULT_ONNX))
    quantize_parser.add_argument("--output", default=str(DEFAULT_ESPDL))
    quantize_parser.add_argument("--calibration", default="splits/val.txt")
    quantize_parser.add_argument("--calibration-images", type=int, default=200)
    quantize_parser.add_argument(
        "--error-report",
        action="store_true",
        help="Run slow graph and layer quantization error analysis.",
    )
    quantize_parser.set_defaults(handler=quantize)

    deploy_parser = subparsers.add_parser(
        "deploy", help="Copy an ESP-DL model into the firmware model directory."
    )
    deploy_parser.add_argument("--model", default=str(DEFAULT_ESPDL))
    deploy_parser.add_argument("--destination", default=str(DEFAULT_DEPLOY_PATH))
    deploy_parser.set_defaults(handler=deploy)

    return parser


def main() -> None:
    os.chdir(SCRIPT_DIR)
    args = build_parser().parse_args()
    args.handler(args)


if __name__ == "__main__":
    main()
