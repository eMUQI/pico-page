#!/usr/bin/env python3
import hashlib
from pathlib import Path

from fontTools import subset
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / ".local/card/Fonts/LXGWWenKai-Regular.ttf"
SOURCE_SHA256 = "8d6ba638ac9553413354cfaab97637c1cd778444e259441ea1e5f8fb2c697fba"
TARGET = ROOT / "web/public/assets/fonts/PicoPageWenKaiSample.woff"
TEXT = "霞鹜文楷 Regular读自己的书，接着读下去。鄱阳湖畔䶮行"


def main():
    if hashlib.sha256(SOURCE.read_bytes()).hexdigest() != SOURCE_SHA256:
        raise ValueError(f"{SOURCE} is not the pinned LXGW WenKai Regular v1.520")
    font = TTFont(SOURCE, recalcTimestamp=False)
    requested = set(map(ord, TEXT))
    missing = requested - set(font.getBestCmap())
    if missing:
        raise ValueError("Font lacks requested characters: " + ", ".join(f"U+{c:04X}" for c in sorted(missing)))
    options = subset.Options()
    options.layout_features = []
    # Preserve the copyright and OFL license metadata in the subset.
    options.name_IDs = [0, 1, 2, 3, 4, 5, 6, 13, 14]
    options.name_legacy = True
    options.name_languages = ["*"]
    sub = subset.Subsetter(options=options)
    sub.populate(unicodes=requested)
    sub.subset(font)
    names = {1: "Pico Page WenKai Sample", 3: "PicoPageWenKaiSample-1.520", 4: "Pico Page WenKai Sample", 6: "PicoPageWenKaiSample"}
    for record in font["name"].names:
        if record.nameID in names:
            record.string = names[record.nameID].encode(record.getEncoding())
    font.flavor = "woff"
    TARGET.parent.mkdir(parents=True, exist_ok=True)
    font.save(TARGET)
    print(f"Generated {TARGET}: {len(requested)} characters, {TARGET.stat().st_size} bytes")


if __name__ == "__main__":
    main()
