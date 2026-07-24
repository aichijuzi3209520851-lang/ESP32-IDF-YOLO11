"""
云端训练包打包脚本
==================
将本地生成的数据集打包成 tar.gz 分卷，用于上传到 AutoDL 云 GPU。

用法:
  python pack_for_cloud.py esp32    # 打包 ESP32 2类数据集
  python pack_for_cloud.py pc       # 打包 PC 5类数据集
"""

import argparse
import hashlib
import os
import shutil
import tarfile
from pathlib import Path

BASE = Path(__file__).resolve().parent
DATASETS_DIR = BASE / "datasets"

# 分卷大小 (约 450MB，避免 AutoDL 网页上传限制)
CHUNK_SIZE = 450 * 1024 * 1024

# 云端训练需要携带的文件
CLOUD_FILES = [
    "train_cloud.py",            # 云端训练脚本
    "requirements-cloud.txt",     # pip 依赖
    "audit_labels.py",            # 标签检查脚本
]


def compute_sha256(file_path: Path) -> str:
    """计算文件 SHA256"""
    sha = hashlib.sha256()
    with open(file_path, "rb") as f:
        while True:
            chunk = f.read(8192)
            if not chunk:
                break
            sha.update(chunk)
    return sha.hexdigest()


def pack_dataset(dataset_name: str, dataset_dir: Path, output_dir: Path) -> None:
    """
    打包数据集为 tar.gz 分卷

    结构:
      yolo11-{dataset_name}-cloud/
        ├── datasets/{dataset_name}/    ← 数据集
        ├── train_cloud.py             ← 训练脚本
        ├── requirements-cloud.txt     ← 依赖
        └── audit_labels.py            ← 标签检查
    """
    output_dir.mkdir(parents=True, exist_ok=True)

    tar_name = f"yolo11-{dataset_name}-cloud"
    temp_dir = output_dir / tar_name
    if temp_dir.exists():
        shutil.rmtree(temp_dir)
    temp_dir.mkdir()

    # 1. 复制数据集
    dataset_dst = temp_dir / "datasets" / dataset_name
    dataset_dst.parent.mkdir(parents=True, exist_ok=True)
    print(f"Copying dataset from {dataset_dir} to {dataset_dst}...")
    shutil.copytree(dataset_dir, dataset_dst, symlinks=False)

    # 2. 复制训练脚本和依赖
    for filename in CLOUD_FILES:
        src = BASE / filename
        if not src.exists():
            print(f"  WARNING: {filename} not found, skipping")
            continue
        dst = temp_dir / filename
        shutil.copy2(src, dst)
        print(f"  Copied: {filename}")

    # 3. 创建 tar.gz
    tar_path = output_dir / f"{tar_name}.tar.gz"
    print(f"\nCreating {tar_path}...")
    with tarfile.open(tar_path, "w:gz") as tar:
        tar.add(temp_dir, arcname=tar_name)

    # 4. 检查大小，决定是否分卷
    size = tar_path.stat().st_size
    size_mb = size / (1024 * 1024)
    print(f"  Size: {size_mb:.1f} MB")

    if size <= CHUNK_SIZE:
        # 不需要分卷
        sha = compute_sha256(tar_path)
        print(f"  SHA256: {sha}")
        # 保存 SHA256
        (output_dir / f"{tar_name}.sha256").write_text(sha)
        print(f"\n  上传文件: {tar_path}")
        print(f"  校验文件: {output_dir / f'{tar_name}.sha256'}")
    else:
        # 需要分卷（Windows 用 split 命令）
        print(f"\n  文件超过 450MB，需要分卷上传。")
        print(f"  请在 PowerShell 中运行：")
        print(f"  cd {output_dir}")
        print(f"  $file = '{tar_name}.tar.gz'")
        print(f"  $chunkSize = 450MB")
        print(f"  $stream = [System.IO.File]::OpenRead($file)")
        print(f"  $buffer = New-Object byte[] $chunkSize")
        print(f"  $i = 0")
        print(f"  while (($read = $stream.Read($buffer, 0, $buffer.Length)) -gt 0) {{")
        print(f"      $chunkFile = '{tar_name}.part' + $i.ToString('D3')")
        print(f"      [System.IO.File]::WriteAllBytes((Join-Path '.' $chunkFile), $buffer[0..($read-1)])")
        print(f"      $i++")
        print(f"  }}")
        print(f"  $stream.Close()")

    # 清理临时目录
    shutil.rmtree(temp_dir)


def main():
    parser = argparse.ArgumentParser(description="打包云端训练数据集")
    parser.add_argument(
        "model",
        choices=["esp32", "pc"],
        help="esp32: ESP32 2类模型, pc: PC 5类模型",
    )
    parser.add_argument(
        "--output",
        default=None,
        help="输出目录 (默认: yolov11/cloud_packages/)",
    )
    args = parser.parse_args()

    if args.model == "esp32":
        dataset_name = "esp32_2class"
        dataset_dir = DATASETS_DIR / "esp32_2class"
    else:
        dataset_name = "pc_5class"
        dataset_dir = DATASETS_DIR / "pc_5class"

    if not dataset_dir.exists():
        print(f"ERROR: 数据集目录不存在: {dataset_dir}")
        print(f"请先运行: python generate_dual_datasets.py")
        return

    output_dir = Path(args.output) if args.output else (BASE / "cloud_packages")
    pack_dataset(dataset_name, dataset_dir, output_dir)

    print(f"\n完成！上传包在: {output_dir}")


if __name__ == "__main__":
    main()