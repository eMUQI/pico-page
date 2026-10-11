"""Generate original EPUB inputs for the native reader's integration check."""

import argparse
import struct
import zipfile
from pathlib import Path
from make_epub_fixture import fixture_entries, write_fixture


def generate(directory: Path) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "sample.txt").write_bytes(("\ufeff" + "甲" * 16384 + "\r\n第二段😀\n" +
                                            "长段文字。" * 12000).encode("utf-8"))
    (directory / "invalid.txt").write_bytes(b"valid\xffinvalid")
    (directory / "gb18030.txt").write_bytes(("甲" * 24575 + "😀\ue587" + "乙" * 500 + "\r\n末尾").encode("gb18030"))
    write_fixture(directory / "valid.epub")
    for kind, mime in (
        ("crlf", "application/epub+zip\r\n"),
        ("padding", " \t\r\napplication/epub+zip\n\r\t "),
        ("limit", " " * 22 + "application/epub+zip" + " " * 22),
        ("oversized", " " * 45 + "application/epub+zip"),
        ("invalid", "application/zip"),
        ("internal-space", "application/epub +zip"),
        ("nul", "application/epub+zip\x00"),
        ("empty", ""),
        ("missing", None),
    ):
        entries = fixture_entries()
        if mime is None:
            del entries["mimetype"]
        else:
            entries["mimetype"] = mime
        write_fixture(directory / ("mimetype-" + kind + ".epub"), entries)
    damaged = directory / "mimetype-read-failure.epub"
    write_fixture(damaged)
    damaged.write_bytes(damaged.read_bytes().replace(b"application/epub+zip", b"application/epub+ziX", 1))
    with zipfile.ZipFile(directory / "valid.epub") as archive:
        central = archive.start_dir
    # Mark the first entry (mimetype) as a DOS directory while retaining its nonzero size.
    data = bytearray((directory / "valid.epub").read_bytes())
    struct.pack_into("<I", data, central + 38, 0x10)
    (directory / "mimetype-directory.epub").write_bytes(data)
    for kind, label in (("empty-label", ""), ("whitespace-label", " \t\n "),
                        ("long-label", "甲" * 10922)):
        labeled = fixture_entries()
        labeled["EPUB/nav.xhtml"] = labeled["EPUB/nav.xhtml"].replace('>第一章</a>', '>' + label + '</a>')
        write_fixture(directory / (kind + ".epub"), labeled)
    nonlinear = fixture_entries()
    nonlinear["EPUB/package.opf"] = nonlinear["EPUB/package.opf"].replace('<itemref idref="two"/>', '<itemref idref="two" linear="no"/>')
    write_fixture(directory / "nonlinear-footer.epub", nonlinear)
    entries = fixture_entries()
    entries["META-INF/container.xml"] = entries["META-INF/container.xml"].replace(
        "EPUB/package.opf", "EPUB/缺失的书目.opf")
    write_fixture(directory / "missing-chinese-resource.epub", entries)
    entries = fixture_entries()
    entries["EPUB/nav.xhtml"] = entries["EPUB/nav.xhtml"].replace(
        '<li><a href="chapter1.xhtml">第一章</a></li>',
        '<li><a href="chapter1.xhtml">第一章：长标题检查目录折行与右侧展开热区不能重叠</a>'
        '<ol><li><a href="chapter1.xhtml#mixed">内联文字</a></li>'
        '<li><a href="chapter1.xhtml#note">脚注位置</a></li></ol></li>')
    write_fixture(directory / "nested-toc.epub", entries)
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
    for kind in ("missing", "oversized", "unsupported"):
        broken = dict(entries)
        if kind == "missing":
            del broken["EPUB/cover.png"]
        elif kind == "oversized":
            broken["EPUB/cover.png"] = entries["EPUB/large.png"]
        else:
            broken["EPUB/cover.png"] = b"GIF89a unsupported cover"
        write_fixture(directory / (kind + "-image-cover.epub"), broken)
    for kind in ("epub3", "epub2", "guide", "unsafe"):
        declared = dict(entries)
        declared["EPUB/chapter1.xhtml"] = fixture_entries()["EPUB/chapter1.xhtml"]
        declared["EPUB/cover.xhtml"] = entries["EPUB/chapter1.xhtml"]
        package = next(name for name in declared if name.endswith(".opf"))
        image = '<item id="cover" href="cover.png" media-type="image/png"'
        if kind == "epub3":
            image += ' properties="cover-image"'
        if kind == "unsafe":
            image = image.replace('href="cover.png"', 'href="../../outside.png"')
        declared[package] = declared[package].replace('</manifest>', image + '/></manifest>')
        if kind in ("epub2", "unsafe"):
            declared[package] = declared[package].replace('</metadata>', '<meta name="cover" content="cover"/></metadata>')
        if kind == "guide":
            declared[package] = declared[package].replace('</package>', '<guide><reference type="cover" href="cover.xhtml"/></guide></package>')
        write_fixture(directory / (kind + "-cover.epub"), declared)
    entries["EPUB/chapter1.xhtml"] = '<html xmlns="http://www.w3.org/1999/xhtml"><body><svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" viewBox="0 0 2 2"><image width="2" height="2" xlink:href="cover.png"/></svg></body></html>'
    write_fixture(directory / "svg-cover.epub", entries)
    plain_svg = entries["EPUB/chapter1.xhtml"]
    entries["EPUB/chapter1.xhtml"] = plain_svg.replace('viewBox=', 'version="1.1" width="100%" height="100%" preserveAspectRatio="none" viewBox=')
    write_fixture(directory / "calibre-svg-cover.epub", entries)
    background = '<html xmlns="http://www.w3.org/1999/xhtml"><body style="background-image:url(\'cover.png\');background-size: contain;">\n%s</body></html>'
    entries["EPUB/chapter1.xhtml"] = background % ""
    write_fixture(directory / "background-page.epub", entries)
    entries["EPUB/chapter1.xhtml"] = background % "<p>有正文的页面。</p>"
    write_fixture(directory / "background-text.epub", entries)
    xhtml11 = '<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.1//EN" "http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd"><html xmlns="http://www.w3.org/1999/xhtml"><body><p>%s</p></body></html>'
    entries["EPUB/chapter1.xhtml"] = xhtml11 % "甲&nbsp;乙&mdash;丙&eacute;"
    write_fixture(directory / "named-entities.epub", entries)
    entries["EPUB/chapter1.xhtml"] = xhtml11 % "甲&unknown;乙"
    write_fixture(directory / "unknown-entity.epub", entries)
    entries["EPUB/chapter1.xhtml"] = plain_svg
    entries["EPUB/chapter1.xhtml"] = entries["EPUB/chapter1.xhtml"].replace('<image width=', '<image transform="rotate(30)" width=')
    write_fixture(directory / "complex-svg-cover.epub", entries)
    entries["EPUB/chapter1.xhtml"] = '<html xmlns="http://www.w3.org/1999/xhtml"><body><p>图片前的正文。</p><img src="cover.png" alt="真实图片"/><p>图片后的正文。</p><img src="missing.png" alt="缺失图片"/><p>末尾正文。</p></body></html>'
    write_fixture(directory / "image-flow.epub", entries)
    # 2000 blocks of 30 scalars each, separator included, end the first part exactly at its
    # limit, so the image after them is the first content of the second part.
    boundary = dict(entries)
    boundary["EPUB/chapter1.xhtml"] = ('<html xmlns="http://www.w3.org/1999/xhtml"><body>' + "".join(
        f"<p>{number:04d}这一段文字用于把第一个分段正好填满到分段上限为止。</p>" for number in range(2000)) +
        '<img src="cover.png" alt="分段首图"/><p>图片之后的正文。</p></body></html>')
    write_fixture(directory / "part-boundary-image.epub", boundary)
    # CSS that removes elements from the layout: the first chapter hides its image, the second
    # only changes how its image is displayed.
    hidden = dict(entries)
    hidden["EPUB/chapter1.xhtml"] = ('<html xmlns="http://www.w3.org/1999/xhtml"><body><p>隐藏图片之前的正文。</p>'
                                     '<img src="cover.png" alt="隐藏图片" style="display:none"/><p>隐藏图片之后的正文。</p></body></html>')
    hidden["EPUB/chapter2.xhtml"] = ('<html xmlns="http://www.w3.org/1999/xhtml"><body><p>块级图片之前的正文。</p>'
                                     '<img src="cover.png" alt="块级图片" style="display: block"/><p>块级图片之后的正文。</p></body></html>')
    write_fixture(directory / "hidden-image.epub", hidden)
    # A table keeps its whole chapter outside the exact KOReader position mapping.
    table = fixture_entries()
    table["EPUB/chapter1.xhtml"] = ('<html xmlns="http://www.w3.org/1999/xhtml"><body><p>表格之前的正文。</p>'
                                    '<table><tr><td>单元格</td></tr></table><p>表格之后的正文。</p></body></html>')
    write_fixture(directory / "table-chapter.epub", table)
    # Image and rule placements for the KOReader position mapping; the reference engine check
    # reads the same file. Block containers mix images with paragraphs and bare text on purpose.
    placements = dict(entries)
    placements["EPUB/chapter1.xhtml"] = '''<html xmlns="http://www.w3.org/1999/xhtml"><head><title>图片</title></head><body>
<h1>图片与正文</h1>
<p>段落文字之前。<img src="cover.png" alt="行内图片"/>段落文字之后。</p>
<p><img src="cover.png" alt="独占段落"/></p>
<p>
  <img src="cover.png" alt="前后有空白"/>
  空白之后的文字。</p>
<div><img src="cover.png" alt="块内图片"/></div>
<div>
<img src="cover.png" alt="块内首图"/>
<p>块内段落。</p>
<img src="cover.png" alt="段间图片"/>
<p>块内第二段。</p>
</div>
<img src="cover.png" alt="正文层图片"/>
<p>正文层图片之后。</p>
<hr/>
<p>段内分隔线之前。<hr/>段内分隔线之后。</p>
<p>分隔线之后。<a href="chapter2.xhtml"><img src="cover.png" alt="链接图片"/></a>结尾。</p>
<ul><li><img src="cover.png" alt="列表图片"/>列表文字。</li></ul>
<p><img src="cover.png" alt="连续一"/> <img src="cover.png" alt="连续二"/>
<img src="cover.png" alt="连续三"/>三图之后。</p>
<p>''' + "图片之后的长段落用于分出多页。" * 120 + '''</p>
<p><em><img src="cover.png" alt="强调内图片"/>强调文字。</em></p>
</body></html>'''
    write_fixture(directory / "image-mapping.epub", placements)
    entries["EPUB/chapter1.xhtml"] = '<html><body><p>第一章正文。</p></body></html>'
    entries["EPUB/chapter2.xhtml"] = '<html><body><img src="cover.png" alt="章节图片"/><p>第二章正文。</p></body></html>'
    write_fixture(directory / "next-chapter-image.epub", entries)
    entries = fixture_entries()
    entries["EPUB/chapter1.xhtml"] = ('<html xmlns="http://www.w3.org/1999/xhtml"><body>' +
                                      "<p>这一章用于检查长章节的回翻与页起点。</p>" * 2000 + "</body></html>")
    write_fixture(directory / "long-chapter.epub", entries)
    entries = fixture_entries()
    entries["EPUB/chapter1.xhtml"] = ('<html xmlns="http://www.w3.org/1999/xhtml"><body>' + "".join(
        f'<p{" id=" + chr(34) + "late" + chr(34) if number == 4500 else ""}>第{number}段：这一段用于检查长章节按段落边界分成多个部分。</p>'
        for number in range(5000)) + "</body></html>")
    entries["EPUB/nav.xhtml"] = entries["EPUB/nav.xhtml"].replace(
        '<li><a href="chapter2.xhtml">', '<li><a href="chapter1.xhtml#late">后段</a></li><li><a href="chapter2.xhtml">')
    write_fixture(directory / "multi-part.epub", entries)
    swipe = dict(entries)
    swipe["EPUB/nav.xhtml"] = swipe["EPUB/nav.xhtml"].replace(
        '<li><a href="chapter1.xhtml">第一章</a></li>',
        "".join(f'<li><a href="chapter1.xhtml">目录条目 {number}</a></li>' for number in range(30)))
    write_fixture(directory / "swipe.epub", swipe)
    entries["EPUB/chapter1.xhtml"] = entries["EPUB/chapter1.xhtml"].replace("<p>第0段", '<p><a href="#late">后段</a>第0段', 1)
    write_fixture(directory / "multi-part-link.epub", entries)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    generate(parser.parse_args().directory)
