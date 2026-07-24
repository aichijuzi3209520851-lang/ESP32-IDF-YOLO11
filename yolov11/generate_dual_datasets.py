"""
双模型数据集生成器
===================
从原始6类数据生成两套独立数据集：

  ESP32轻量模型 (2类):
    0: stain        ← bird_drop + dust
    1: damage       ← electrical_damage + physical_damage
    clean → 移除标注框（作为负样本）
    snow_covered → 排除

  PC高精度模型 (5类):
    0: bird_drop
    1: dust
    2: electrical_damage
    3: physical_damage
    4: snow_covered
    clean → 移除标注框（作为负样本）

输出目录结构:
  datasets/esp32_2class/
    ├── data.yaml
    ├── train/images/  → 软链接到原始图片
    ├── train/labels/  → 重新映射的标签
    ├── val/images/
    ├── val/labels/
    ├── test/images/
    ├── test/labels/
    └── splits/{train,val,test}.txt

  datasets/pc_5class/
    └── (同上结构)
"""

import os
import shutil
from collections import Counter
from pathlib import Path

BASE = Path(__file__).resolve().parent
SPLITS_DIR = BASE / "splits"
ORIGINAL_DATA_YAML = BASE / "data.yaml"

# ============================================================
# 类别映射定义
# ============================================================

# 原始6类: 0=bird_drop, 1=clean, 2=dust, 3=electrical_damage, 4=physical_damage, 5=snow_covered

# ESP32 2类映射: 原始class_id → (新class_id, 新类名) 或 None=移除框, "EXCLUDE"=排除整张图
ESP32_CLASS_MAP = {
    0: (0, "stain"),        # bird_drop → stain
    1: None,                 # clean → 移除框（负样本）
    2: (0, "stain"),        # dust → stain
    3: (1, "damage"),       # electrical_damage → damage
    4: (1, "damage"),       # physical_damage → damage
    5: "EXCLUDE",           # snow_covered → 排除整张图
}
ESP32_CLASS_NAMES = ["stain", "damage"]

# PC 5类映射: 原始class_id → (新class_id, 新类名) 或 None=移除框
PC_CLASS_MAP = {
    0: (0, "bird_drop"),         # bird_drop → 0
    1: None,                      # clean → 移除框（负样本）
    2: (1, "dust"),              # dust → 1
    3: (2, "electrical_damage"), # electrical_damage → 2
    4: (3, "physical_damage"),   # physical_damage → 3
    5: (4, "snow_covered"),      # snow_covered → 4
}
PC_CLASS_NAMES = ["bird_drop", "dust", "electrical_damage", "physical_damage", "snow_covered"]


def read_split_manifest(split_name: str) -> list[str]:
    """读取 splits/{split_name}.txt，返回相对路径列表"""
    manifest = SPLITS_DIR / f"{split_name}.txt"
    if not manifest.exists():
        raise FileNotFoundError(f"Split manifest not found: {manifest}")
    lines = [line.strip() for line in manifest.read_text("utf-8").splitlines() if line.strip()]
    print(f"  {split_name}: {len(lines)} images")
    return lines


def make_output_dirs(dataset_dir: Path, split_names: list[str]) -> None:
    """创建输出目录结构"""
    for split_name in split_names:
        (dataset_dir / split_name / "images").mkdir(parents=True, exist_ok=True)
        (dataset_dir / split_name / "labels").mkdir(parents=True, exist_ok=True)
    (dataset_dir / "splits").mkdir(parents=True, exist_ok=True)


def process_dataset(
    dataset_name: str,
    class_map: dict,
    class_names: list[str],
    dataset_dir: Path,
) -> None:
    """
    处理数据集：重新映射标签，生成新目录结构

    Args:
        dataset_name: "ESP32-2class" 或 "PC-5class"
        class_map: 原始class_id → 新映射
        class_names: 新类别名称列表
        dataset_dir: 输出目录
    """
    print(f"\n{'='*60}")
    print(f"  生成 {dataset_name} 数据集")
    print(f"{'='*60}")

    split_names = ["train", "val", "test"]
    make_output_dirs(dataset_dir, split_names)

    stats = {s: {"images": 0, "excluded": 0, "negative": 0, "boxes": Counter()} for s in split_names}

    for split_name in split_names:
        image_paths = read_split_manifest(split_name)
        new_manifest_lines = []

        for rel_path in image_paths:
            # 原始文件路径
            image_path = BASE / rel_path
            rel_dir = os.path.dirname(rel_path)
            image_name = os.path.basename(rel_path)
            stem = Path(image_name).stem
            label_name = f"{stem}.txt"

            # 原始标签路径: train/images/xxx.jpg → train/labels/xxx.txt
            original_label = BASE / rel_dir.replace("images", "labels") / label_name

            if not original_label.exists():
                print(f"  WARNING: Missing label for {rel_path}, skipping")
                continue

            # 读取原始标签
            lines = original_label.read_text("utf-8").splitlines()
            new_lines = []
            exclude_image = False

            for line in lines:
                line = line.strip()
                if not line:
                    continue
                parts = line.split()
                if len(parts) != 5:
                    continue
                orig_class_id = int(parts[0])
                coords = parts[1:]

                mapping = class_map.get(orig_class_id)

                if mapping == "EXCLUDE":
                    # 排除整张图（如 ESP32 的 snow_covered）
                    exclude_image = True
                    break
                elif mapping is None:
                    # 移除该框（如 clean 类）
                    continue
                else:
                    # 重新映射
                    new_class_id, new_class_name = mapping
                    new_lines.append(f"{new_class_id} {' '.join(coords)}")
                    stats[split_name]["boxes"][new_class_name] += 1

            if exclude_image:
                stats[split_name]["excluded"] += 1
                continue

            # 创建符号链接到原始图片（避免复制占用磁盘空间）
            dst_image = dataset_dir / split_name / "images" / image_name
            if not dst_image.exists():
                try:
                    # Windows 上 symlink 可能失败，回退到 copy
                    os.symlink(image_path, dst_image)
                except OSError:
                    shutil.copy2(image_path, dst_image)

            # 写入新标签文件
            dst_label = dataset_dir / split_name / "labels" / label_name
            if new_lines:
                dst_label.write_text("\n".join(new_lines) + "\n", encoding="utf-8")
            else:
                # 没有框的负样本，写空文件
                dst_label.write_text("", encoding="utf-8")
                stats[split_name]["negative"] += 1

            # 记录相对路径到 manifest
            new_rel = f"{split_name}/images/{image_name}"
            new_manifest_lines.append(new_rel)

        # 写入 split manifest
        manifest_path = dataset_dir / "splits" / f"{split_name}.txt"
        manifest_path.write_text("\n".join(new_manifest_lines) + "\n", encoding="utf-8")

        stats[split_name]["images"] = len(new_manifest_lines)

    # 打印统计
    print(f"\n  --- {dataset_name} 数据集统计 ---")
    for split_name in split_names:
        s = stats[split_name]
        print(f"  {split_name}: {s['images']} images "
              f"(排除{s['excluded']}, 负样本{s['negative']})")
        if s["boxes"]:
            box_counts = ", ".join(f"{k}={v}" for k, v in s["boxes"].items())
            print(f"    boxes: {box_counts}")

    # 写入 data.yaml
    data_yaml = dataset_dir / "data.yaml"
    data_yaml.write_text(
        f"path: datasets/{dataset_dir.name}\n"
        f"train: train/images\n"
        f"val: val/images\n"
        f"test: test/images\n"
        f"\n"
        f"nc: {len(class_names)}\n"
        f"names:\n" +
        "\n".join(f"  {i}: {name}" for i, name in enumerate(class_names)) +
        "\n",
        encoding="utf-8",
    )
    print(f"\n  data.yaml: {data_yaml}")
    print(f"  classes: {class_names}")


def main():
    print("=" * 60)
    print("  双模型数据集生成器")
    print("=" * 60)
    print(f"  原始数据: {BASE}")
    print(f"  原始类别: 6类 (bird_drop, clean, dust, electrical_damage, physical_damage, snow_covered)")

    # 1. 生成 ESP32 2类数据集
    esp32_dir = BASE / "datasets" / "esp32_2class"
    if esp32_dir.exists():
        shutil.rmtree(esp32_dir)
    process_dataset("ESP32-2class", ESP32_CLASS_MAP, ESP32_CLASS_NAMES, esp32_dir)

    # 2. 生成 PC 5类数据集
    pc_dir = BASE / "datasets" / "pc_5class"
    if pc_dir.exists():
        shutil.rmtree(pc_dir)
    process_dataset("PC-5class", PC_CLASS_MAP, PC_CLASS_NAMES, pc_dir)

    print(f"\n{'='*60}")
    print("  数据集生成完成！")
    print(f"{'='*60}")
    print(f"  ESP32 2类: {esp32_dir}")
    print(f"  PC 5类:    {pc_dir}")
    print(f"\n  下一步：")
    print(f"  1. 打包 ESP32 数据集: 运行 pack_for_cloud.py esp32")
    print(f"  2. 打包 PC 数据集:    运行 pack_for_cloud.py pc")
    print(f"  3. 上传到 AutoDL 云端训练")


if __name__ == "__main__":
    main()
