"""Generate deterministic contact sheets for manual YOLO label review."""

import csv
import math
import random
from collections import defaultdict
from pathlib import Path

import yaml
from PIL import Image, ImageDraw, ImageFont, ImageOps


SEED = 42
SAMPLES_PER_CLASS = 50
TILE_SIZE = (240, 200)
SHEET_COLUMNS = 5
IMAGE_SUFFIXES = {".jpg", ".jpeg", ".png", ".bmp"}

BASE = Path(__file__).resolve().parent
OUTPUT_DIR = BASE / "audit"


def load_class_names() -> list[str]:
    with (BASE / "data.yaml").open("r", encoding="utf-8") as stream:
        names = yaml.safe_load(stream)["names"]
    if isinstance(names, dict):
        return [names[index] for index in sorted(names)]
    return list(names)


def collect_boxes(class_count: int) -> dict[int, list[dict]]:
    boxes = defaultdict(list)
    for split_name in ("train", "val"):
        image_dir = BASE / split_name / "images"
        label_dir = BASE / split_name / "labels"
        for image in sorted(image_dir.iterdir()):
            if image.suffix.lower() not in IMAGE_SUFFIXES:
                continue
            label = label_dir / f"{image.stem}.txt"
            source_id = image.stem.split(".rf.", 1)[0]
            for line_number, line in enumerate(
                label.read_text(encoding="utf-8").splitlines(), start=1
            ):
                parts = line.split()
                class_id = int(parts[0])
                if class_id < 0 or class_id >= class_count:
                    raise ValueError(f"Class {class_id} out of range at {label}:{line_number}")
                boxes[class_id].append({
                    "image": image,
                    "label": label,
                    "line": line_number,
                    "source_id": source_id,
                    "bbox": tuple(float(value) for value in parts[1:]),
                })
    return boxes


def choose_samples(records: list[dict]) -> list[dict]:
    rng = random.Random(SEED)
    shuffled = list(records)
    rng.shuffle(shuffled)
    unique_sources = []
    repeated_sources = []
    seen = set()
    for record in shuffled:
        if record["source_id"] in seen:
            repeated_sources.append(record)
        else:
            seen.add(record["source_id"])
            unique_sources.append(record)
    return (unique_sources + repeated_sources)[:SAMPLES_PER_CLASS]


def render_tile(record: dict, class_id: int, class_name: str) -> Image.Image:
    tile = Image.new("RGB", TILE_SIZE, "white")
    label_height = 28
    available_size = (TILE_SIZE[0], TILE_SIZE[1] - label_height)

    with Image.open(record["image"]) as source:
        source = source.convert("RGB")
        original_width, original_height = source.size
        preview = ImageOps.contain(source, available_size)

    offset_x = (TILE_SIZE[0] - preview.width) // 2
    offset_y = label_height + (available_size[1] - preview.height) // 2
    tile.paste(preview, (offset_x, offset_y))

    center_x, center_y, width, height = record["bbox"]
    scale_x = preview.width / original_width
    scale_y = preview.height / original_height
    x1 = offset_x + (center_x - width / 2) * original_width * scale_x
    y1 = offset_y + (center_y - height / 2) * original_height * scale_y
    x2 = offset_x + (center_x + width / 2) * original_width * scale_x
    y2 = offset_y + (center_y + height / 2) * original_height * scale_y

    draw = ImageDraw.Draw(tile)
    draw.rectangle((x1, y1, x2, y2), outline="red", width=3)
    title = f"{class_id}:{class_name} {record['source_id'][:20]}"
    draw.text((4, 6), title, fill="black", font=ImageFont.load_default())
    return tile


def render_sheet(records: list[dict], class_id: int, class_name: str) -> Path:
    rows = math.ceil(len(records) / SHEET_COLUMNS)
    sheet = Image.new(
        "RGB",
        (TILE_SIZE[0] * SHEET_COLUMNS, TILE_SIZE[1] * rows),
        "#dddddd",
    )
    for index, record in enumerate(records):
        tile = render_tile(record, class_id, class_name)
        x = (index % SHEET_COLUMNS) * TILE_SIZE[0]
        y = (index // SHEET_COLUMNS) * TILE_SIZE[1]
        sheet.paste(tile, (x, y))

    output = OUTPUT_DIR / f"class_{class_id}_{class_name}.jpg"
    sheet.save(output, quality=90)
    return output


def main() -> None:
    class_names = load_class_names()
    boxes = collect_boxes(len(class_names))
    OUTPUT_DIR.mkdir(exist_ok=True)
    csv_rows = []

    for class_id, class_name in enumerate(class_names):
        selected = choose_samples(boxes[class_id])
        if len(selected) < SAMPLES_PER_CLASS:
            raise ValueError(
                f"Class {class_id} has only {len(selected)} boxes; "
                f"need {SAMPLES_PER_CLASS}"
            )
        output = render_sheet(selected, class_id, class_name)
        print(f"{class_id}:{class_name} -> {output} ({len(selected)} boxes)")
        for record in selected:
            csv_rows.append({
                "class_id": class_id,
                "class_name": class_name,
                "image": record["image"].relative_to(BASE).as_posix(),
                "label": record["label"].relative_to(BASE).as_posix(),
                "line": record["line"],
                "source_id": record["source_id"],
                "bbox": " ".join(str(value) for value in record["bbox"]),
            })

    with (OUTPUT_DIR / "samples.csv").open(
        "w", encoding="utf-8", newline=""
    ) as stream:
        writer = csv.DictWriter(stream, fieldnames=csv_rows[0].keys())
        writer.writeheader()
        writer.writerows(csv_rows)


if __name__ == "__main__":
    main()
