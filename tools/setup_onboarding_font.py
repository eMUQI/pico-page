#!/usr/bin/env python3
from pathlib import Path
import json
from fontTools import subset
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[1]
font = TTFont(ROOT / '.local/card/Fonts/LXGWWenKai-Regular.ttf', recalcTimestamp=False)
options = subset.Options()
options.layout_features = []
options.notdef_outline = True
options.name_IDs = [0, 1, 2, 3, 4, 5, 6, 13, 14]
options.name_legacy = True
options.name_languages = ["*"]
sub = subset.Subsetter(options=options)
subtitle = "花一分钟，学会翻页和打开工具。"
text = subtitle
for language in ("zh-Hant", "en"):
    catalog = json.loads((ROOT / f"firmware/components/reader_core/locales/{language}.json").read_text(encoding="utf-8"))
    text += catalog[subtitle]
missing = set(map(ord, text)) - set(font.getBestCmap())
if missing:
    raise ValueError("Welcome font lacks requested characters: " + ", ".join(f"U+{c:04X}" for c in sorted(missing)))
sub.populate(text=text)
sub.subset(font)
for record in font['name'].names:
    names = {1: 'Pico Page Welcome', 3: 'PicoPageWelcome-1', 4: 'Pico Page Welcome', 6: 'PicoPageWelcome'}
    if record.nameID in names:
        record.string = names[record.nameID].encode(record.getEncoding())
font.save(ROOT / 'firmware/components/reader_graphics/fonts/PicoPageWelcome.ttf')
