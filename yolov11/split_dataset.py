"""Create leakage-free YOLO train/val/test manifests without moving source files."""

import os
import random
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path

import yaml


SEED = 42
SPLIT_RATIOS = {"train": 0.70, "val": 0.15, "test": 0.15}
IMAGE_SUFFIXES = {".jpg", ".jpeg", ".png", ".bmp"}

BASE = Path(__file__).resolve().parent
SPLITS_DIR = BASE / "splits"


@dataclass(frozen=True)
class Sample:
    image: Path
    label: Path
    source_id: str
    classes: frozenset[int]


def load_class_names() -> list[str]:
    with (BASE / "data.yaml").open("r", encoding="utf-8") as stream:
        names = yaml.safe_load(stream)["names"]
    if isinstance(names, dict):
        return [names[index] for index in sorted(names)]
    return list(names)


def source_id(image: Path) -> str:
    return image.stem.split(".rf.", 1)[0]


def read_classes(label: Path, class_count: int) -> frozenset[int]:
    classes = set()
    lines = label.read_text(encoding="utf-8").splitlines()
    if not lines:
        raise ValueError(f"Empty label file: {label}")

    for line_number, line in enumerate(lines, start=1):
        parts = line.split()
        if len(parts) != 5:
            raise ValueError(f"Invalid label at {label}:{line_number}: {line}")
        class_id = int(parts[0])
        coords = [float(value) for value in parts[1:]]
        if class_id < 0 or class_id >= class_count:
            raise ValueError(f"Class {class_id} out of range at {label}:{line_number}")
        if any(value < 0.0 or value > 1.0 for value in coords):
            raise ValueError(f"Coordinate out of range at {label}:{line_number}")
        classes.add(class_id)

    return frozenset(classes)


def discover_samples(class_count: int) -> list[Sample]:
    samples = []
    seen_images = set()

    for existing_split in ("train", "val"):
        image_dir = BASE / existing_split / "images"
        label_dir = BASE / existing_split / "labels"
        for image in sorted(image_dir.iterdir()):
            if image.suffix.lower() not in IMAGE_SUFFIXES:
                continue
            if image.name in seen_images:
                raise ValueError(f"Duplicate image name: {image.name}")
            seen_images.add(image.name)

            label = label_dir / f"{image.stem}.txt"
            if not label.exists():
                raise FileNotFoundError(f"Missing label for {image}")
            samples.append(Sample(
                image=image,
                label=label,
                source_id=source_id(image),
                classes=read_classes(label, class_count),
            ))

    if not samples:
        raise ValueError("No dataset images found")
    return samples


def assign_groups(samples: list[Sample], class_count: int) -> dict[str, list[Sample]]:
    groups = defaultdict(list)
    for sample in samples:
        groups[sample.source_id].append(sample)

    total_class_images = Counter()
    for sample in samples:
        total_class_images.update(sample.classes)

    rng = random.Random(SEED)
    ordered_groups = list(groups.items())
    rng.shuffle(ordered_groups)

    def rarity(item):
        _, group_samples = item
        group_counts = Counter()
        for sample in group_samples:
            group_counts.update(sample.classes)
        return sum(
            count / total_class_images[class_id]
            for class_id, count in group_counts.items()
        )

    ordered_groups.sort(
        key=lambda item: (rarity(item), len(item[1])),
        reverse=True,
    )

    target_images = {
        name: len(samples) * ratio for name, ratio in SPLIT_RATIOS.items()
    }
    target_classes = {
        name: {
            class_id: total_class_images[class_id] * ratio
            for class_id in range(class_count)
        }
        for name, ratio in SPLIT_RATIOS.items()
    }
    assigned = {name: [] for name in SPLIT_RATIOS}
    assigned_classes = {name: Counter() for name in SPLIT_RATIOS}

    for _, group_samples in ordered_groups:
        group_classes = Counter()
        for sample in group_samples:
            group_classes.update(sample.classes)

        def placement_score(split_name):
            image_fill = (
                len(assigned[split_name]) + len(group_samples)
            ) / target_images[split_name]
            class_fills = [
                (assigned_classes[split_name][class_id] + count)
                / target_classes[split_name][class_id]
                for class_id, count in group_classes.items()
            ]
            class_fill = sum(class_fills) / len(class_fills)
            overflow = max(0.0, image_fill - 1.0) * 10.0
            overflow += sum(max(0.0, fill - 1.0) for fill in class_fills) * 2.0
            return 0.4 * image_fill + 0.6 * class_fill + overflow

        destination = min(SPLIT_RATIOS, key=placement_score)
        assigned[destination].extend(group_samples)
        assigned_classes[destination].update(group_classes)

    return assigned


def write_manifests(assigned: dict[str, list[Sample]]) -> None:
    SPLITS_DIR.mkdir(exist_ok=True)
    for split_name, samples in assigned.items():
        lines = [
            Path(os.path.relpath(sample.image, SPLITS_DIR)).as_posix()
            for sample in sorted(samples, key=lambda sample: str(sample.image))
        ]
        (SPLITS_DIR / f"{split_name}.txt").write_text(
            "\n".join(lines) + "\n",
            encoding="utf-8",
        )


def validate_and_report(
    assigned: dict[str, list[Sample]], class_names: list[str]
) -> None:
    source_sets = {
        name: {sample.source_id for sample in samples}
        for name, samples in assigned.items()
    }
    split_names = list(SPLIT_RATIOS)
    for index, left in enumerate(split_names):
        for right in split_names[index + 1:]:
            overlap = source_sets[left] & source_sets[right]
            if overlap:
                raise ValueError(f"Source leakage between {left} and {right}: {len(overlap)}")

    total = sum(len(samples) for samples in assigned.values())
    print(f"Generated leakage-free manifests for {total} images (seed={SEED})")
    for split_name, samples in assigned.items():
        class_images = Counter()
        for sample in samples:
            class_images.update(sample.classes)
        counts = ", ".join(
            f"{class_names[class_id]}={class_images[class_id]}"
            for class_id in range(len(class_names))
        )
        print(
            f"{split_name}: images={len(samples)} "
            f"sources={len(source_sets[split_name])} {counts}"
        )


def main() -> None:
    class_names = load_class_names()
    samples = discover_samples(len(class_names))
    assigned = assign_groups(samples, len(class_names))
    write_manifests(assigned)
    validate_and_report(assigned, class_names)


if __name__ == "__main__":
    main()
