import os, random, shutil
from pathlib import Path

random.seed(42)
BASE = Path(__file__).parent

TRAIN_IMG = BASE / "train" / "images"
TRAIN_LBL = BASE / "train" / "labels"
VAL_IMG   = BASE / "val" / "images"
VAL_LBL   = BASE / "val" / "labels"

VAL_IMG.mkdir(parents=True, exist_ok=True)
VAL_LBL.mkdir(parents=True, exist_ok=True)

images = sorted([f for f in os.listdir(TRAIN_IMG) if f.lower().endswith(('.jpg','.jpeg','.png'))])
random.shuffle(images)
n_val = int(len(images) * 0.2)

print(f"Total: {len(images)}  Train: {len(images)-n_val}  Val: {n_val}")

for img in images[:n_val]:
    shutil.move(str(TRAIN_IMG / img), str(VAL_IMG / img))
    lbl = Path(img).stem + ".txt"
    src_lbl = TRAIN_LBL / lbl
    if src_lbl.exists():
        shutil.move(str(src_lbl), str(VAL_LBL / lbl))
    else:
        print(f"  WARN: no label for {img}")
print("Done.")
