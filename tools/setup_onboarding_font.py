#!/usr/bin/env python3
"""Subset the checksum-pinned WenKai source for the offline welcome subtitle (OFL)."""
from pathlib import Path
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
sub.populate(text='花一分钟，学会翻页和打开工具。')
sub.subset(font)
for record in font['name'].names:
    names = {1: 'Pico Page Welcome', 3: 'PicoPageWelcome-1', 4: 'Pico Page Welcome', 6: 'PicoPageWelcome'}
    if record.nameID in names:
        record.string = names[record.nameID].encode(record.getEncoding())
font.save(ROOT / 'firmware/components/reader_graphics/fonts/PicoPageWelcome.ttf')
