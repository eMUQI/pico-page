#!/usr/bin/env python3
"""Regenerate the OFL UI subset with fonttools 4.60.1 in a local virtual environment."""
from pathlib import Path
import argparse
from fontTools import subset
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TEXT = (
    "Pico Page TF EPUB K1 K2 K3 .local/PicoPage/fonts/0123456789"
    "未检测到存储卡请插入后重新启动初始化失败无法读取挂载书库为空"
    "返回关闭取消确定重试继续打开选择上一页下一页全刷工具目录排版字号行距边距"
    "字体默认应用阅读书架设置清晰均衡快速保存尚未成功进度已记录损坏不支持"
    "正在准备内容加载当前章节中文英文未知需要电脑处理原书文件版本位置服务网络离线"
    "本地远端同步上传下载冲突本书全局继承恢复睡眠电量刷新暂停繁忙错误帮助"
    "：，。！？（）【】《》、；—…“”‘’「」·－＋←→↑↓％"
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font", type=Path, default=ROOT / ".local/card/PicoPage/fonts/NotoSansSC-Regular.otf")
    parser.add_argument("--strings", type=Path, action="append", default=[])
    args = parser.parse_args()
    text = DEFAULT_TEXT + "".join(chr(c) for c in range(32, 127))
    for path in args.strings:
        # C++ string content and identifiers are harmless extras in this small UI subset.
        text += path.read_text(encoding="utf-8")
    font = TTFont(args.font, recalcTimestamp=False)
    requested = set(map(ord, text)) - set(range(32))
    missing = requested - set(font.getBestCmap())
    if missing:
        raise ValueError("Font lacks requested characters: " + ", ".join(f"U+{c:04X}" for c in sorted(missing)))
    options = subset.Options()
    options.layout_features = []
    options.name_IDs = [0, 1, 2, 3, 4, 5, 6, 13, 14]
    options.name_legacy = True
    options.name_languages = ["*"]
    sub = subset.Subsetter(options=options)
    sub.populate(unicodes=requested)
    sub.subset(font)
    names = {1: "Pico Page Fallback", 2: "Regular", 3: "PicoPageFallback-1", 4: "Pico Page Fallback", 6: "PicoPageFallback"}
    for record in font["name"].names:
        if record.nameID in names:
            record.string = names[record.nameID].encode(record.getEncoding())
    if "CFF " in font:
        cff = font["CFF "].cff
        cff.fontNames = ["PicoPageFallback"]
        cff.topDictIndex[0].FamilyName = "Pico Page Fallback"
        cff.topDictIndex[0].FullName = "Pico Page Fallback"
    target = ROOT / "firmware/components/reader_graphics/fonts/PicoPageFallback.otf"
    font.save(target)
    (target.parent / "fallback_characters.txt").write_text("".join(map(chr, sorted(requested))) + "\n", encoding="utf-8")
    print(f"Generated {target}: {len(requested)} characters, {target.stat().st_size} bytes")


if __name__ == "__main__":
    main()
