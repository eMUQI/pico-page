"""Generate original EPUB inputs for the native reader's integration check."""

import argparse
from pathlib import Path
from make_epub_fixture import fixture_entries, write_fixture


def generate(directory: Path) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    write_fixture(directory / "valid.epub")
    (directory / "malformed.epub").write_bytes(b"This is not a ZIP archive.")
    entries = fixture_entries()
    entries["EPUB/chapter2.xhtml"] = "<html><body>" + "超" * 200001 + "</body></html>"
    write_fixture(directory / "oversized-chapter.epub", entries)
    entries = fixture_entries()
    entries["EPUB/chapter1.xhtml"] = '''<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.1//EN" "http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd"><html xmlns="http://www.w3.org/1999/xhtml">
<head><link rel="stylesheet" href="book.css"/><style>p { margin: 0; }</style></head>
<body><p style="text-align:justify">正文保留 <em>内联</em></p>
<p><img src="image.png" alt="示例图片"/></p></body></html>'''
    entries["EPUB/book.css"] = "p { font-size: 1em; }"
    write_fixture(directory / "styled.epub", entries)
    mapping = dict(entries)
    mapping["EPUB/chapter1.xhtml"] = mapping["EPUB/chapter1.xhtml"].replace(
        '<p><img src="image.png" alt="示例图片"/></p>', '<p> \n<em>起点</em> 尾部</p>')
    write_fixture(directory / "mapping.epub", mapping)
    entries["EPUB/chapter1.xhtml"] = entries["EPUB/chapter1.xhtml"].replace("正文保留", "正文&missing;")
    write_fixture(directory / "unresolved-entity.epub", entries)
    entries = fixture_entries()
    entries["EPUB/chapter1.xhtml"] = '<html xmlns="http://www.w3.org/1999/xhtml"><body><img src="cover.png" alt="封面"/></body></html>'
    entries["EPUB/cover.png"] = bytes.fromhex("89504e470d0a1a0a0000000d494844520000000200000002080600000072b60d2400000006624b474400ff00ff00ffa0bda7930000001549444154789c63606060f80f020cff19408881010048c906fa001f0d6c0000000049454e44ae426082")
    entries["EPUB/large.png"] = b"\x89PNG\r\n\x1a\n" + bytes(range(256)) * 8192
    write_fixture(directory / "image-cover.epub", entries)
    entries["EPUB/chapter1.xhtml"] = '<html xmlns="http://www.w3.org/1999/xhtml"><body><svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" viewBox="0 0 2 2"><image width="2" height="2" xlink:href="cover.png"/></svg></body></html>'
    write_fixture(directory / "svg-cover.epub", entries)
    entries["EPUB/chapter1.xhtml"] = entries["EPUB/chapter1.xhtml"].replace('<image width=', '<image transform="rotate(30)" width=')
    write_fixture(directory / "complex-svg-cover.epub", entries)
    entries["EPUB/chapter1.xhtml"] = '<html xmlns="http://www.w3.org/1999/xhtml"><body><p>图片前的正文。</p><img src="cover.png" alt="真实图片"/><p>图片后的正文。</p><img src="missing.png" alt="缺失图片"/><p>末尾正文。</p></body></html>'
    write_fixture(directory / "image-flow.epub", entries)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    generate(parser.parse_args().directory)
