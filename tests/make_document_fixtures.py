"""Generate original EPUB inputs for the native reader's integration check."""

import argparse
import hashlib
import struct
import zipfile
from pathlib import Path
from make_epub_fixture import fixture_entries, write_fixture


def large_epub_entries(chapters: int) -> dict[str, str | bytes]:
    assert chapters in (300, 3000)
    large = {name: value for name, value in fixture_entries().items()
             if name in ("mimetype", "META-INF/container.xml")}
    manifest = '<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>'
    spine = ''
    points = []
    for volume in range(chapters // 100):
        points.append(f'<navPoint><navLabel><text>卷 {volume}</text></navLabel>'
                      f'<content src="chapter{volume * 100}.xhtml"/>')
        for chapter in range(volume * 100, (volume + 1) * 100):
            manifest += f'<item id="c{chapter}" href="chapter{chapter}.xhtml" media-type="application/xhtml+xml"/>'
            spine += f'<itemref idref="c{chapter}"/>'
            points.append(f'<navPoint><navLabel><text>章 {chapter} &amp; 正文</text></navLabel>'
                          f'<content src="chapter{chapter}.xhtml#body"/></navPoint>')
            large[f'EPUB/chapter{chapter}.xhtml'] = (
                f'<html xmlns="http://www.w3.org/1999/xhtml"><body><p id="body">正文 {chapter}</p></body></html>')
        points.append('</navPoint>')
    large['EPUB/package.opf'] = '<package><spine>' + spine + '</spine><manifest>' + manifest + '</manifest></package>'
    large['EPUB/toc.ncx'] = '<ncx><navMap>' + ''.join(points) + '</navMap></ncx>'
    return large


def generate(directory: Path) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "sample.txt").write_bytes(("\ufeff" + "甲" * 16384 + "\r\n第二段😀\n" +
                                            "长段文字。" * 12000).encode("utf-8"))
    for encoding, bom in (("utf-8", b"\xef\xbb\xbf"), ("gb18030", b""),
                          ("utf-16-le", b"\xff\xfe"), ("utf-16-be", b"\xfe\xff")):
        prefix = "序章 开始\r\n这是一行正文。\n"
        unit_bytes = len("x".encode(encoding))
        padding = (49150 - len(bom) - len((prefix + "\n").encode(encoding))) // unit_bytes
        text = prefix + "x" * padding + "\n第一章 边界😀\r\n" + "边界正文。\n" * 5000 + "Chapter II: Café\n尾声 结束\n"
        (directory / ("toc-" + encoding + ".txt")).write_bytes(bom + text.encode(encoding))
    stress = "".join(f"第{chapter}章 压力测试\n" +
        "".join(f"第{paragraph}段：" + "连续阅读应当保持每页文字与来源位置一致。" * 10 + "\n" for paragraph in range(18))
        for chapter in range(220))
    (directory / "txt-stress.txt").write_text(stress, encoding="utf-8")
    (directory / "invalid.txt").write_bytes(b"valid\xffinvalid")
    for encoding in ("utf-16-le", "utf-16-be"):
        bom = b"\xff\xfe" if encoding.endswith("le") else b"\xfe\xff"
        text = "甲" * 24574 + "😀\r\n第二段" + "乙" * 30000 + "末尾"
        (directory / (encoding + ".txt")).write_bytes(bom + text.encode(encoding))
        (directory / (encoding + "-cr-run.txt")).write_bytes(bom + ("甲" + "\r" * 60000 + "乙").encode(encoding))
        for name, data in (("odd", bom + b"x"), ("high", bom + "\ud800".encode(encoding, "surrogatepass")),
                           ("low", bom + "\udc00".encode(encoding, "surrogatepass")),
                           ("pair", bom + "\ud800甲".encode(encoding, "surrogatepass")),
                           ("nul", bom + "a\x00b".encode(encoding)), ("empty", bom)):
            (directory / (encoding + "-" + name + ".txt")).write_bytes(data)
    (directory / "gb18030.txt").write_bytes(("甲" * 24575 + "😀\ue587" + "乙" * 500 + "\r\n末尾").encode("gb18030"))
    write_fixture(directory / "valid.epub")
    fragments = fixture_entries()
    fragments["EPUB/chapter1.xhtml"] = ('<html xmlns="http://www.w3.org/1999/xhtml"><body>'
        '<p id="z">甲</p><p id="a">乙</p><p>' + 'x' * 60000 +
        '</p><p>分段</p><p id="z">丙</p></body></html>')
    fragments["EPUB/chapter2.xhtml"] = ('<html xmlns="http://www.w3.org/1999/xhtml"><body>' +
        ''.join(f'<p id="target{ordinal}">甲</p>' for ordinal in range(2048, 0, -1)) +
        '<p id="target1024">重复</p></body></html>')
    write_fixture(directory / "fragment-index.epub", fragments)
    html = {name.replace("chapter1.xhtml", "chapter1.html"):
            value.replace("chapter1.xhtml", "chapter1.html") if isinstance(value, str) else value
            for name, value in fixture_entries().items()}
    html["EPUB/chapter1.html"] = ('<!doctype html><html><head><script>var a="<p>&missing;";</script></head>'
        '<body><p disabled title=unquoted>甲&nbsp;&amp;乙 < 3<br>尾\v\f段<p>末尾</body></html>')
    write_fixture(directory / "html-body.epub", html)
    html["EPUB/chapter1.html"] = ('<!DOCTYPE HTML><html><body>' +
        ''.join('<p disabled>甲乙&amp;丁&nbsp;😀' + str(i) for i in range(20000)) + '</body></html>')
    write_fixture(directory / "html-body-parts.epub", html)
    html["EPUB/chapter1.html"] = ('<!DOCTYPE html><html><body>' +
        ''.join('<p>甲乙丁😀' + str(i) + '</p>' for i in range(20000)) +
        '<input disabled><p>尾部</p></body></html>')
    write_fixture(directory / "html-late-repair.epub", html)
    for kind, prefix in (("tree", "<p><div>prefix{}</div></p>"),
                         ("comment", "<p>a<!--boundary-->bb{}</p>"),
                         ("void", "<p><br><span>prefix{}</span></br></p>"),
                         ("namespace", '<p><q:span xmlns:q="urn:source">prefix{}</q:span></p>')):
        html["EPUB/chapter1.html"] = ('<!DOCTYPE html><html><body>' +
            ''.join(prefix.format(i) for i in range(25000)) +
            '<br/><input disabled><p>尾部</p></body></html>')
        write_fixture(directory / ("html-late-" + kind + ".epub"), html)
    html["EPUB/chapter1.html"] = ('<!DOCTYPE html><html><body><p><![CDATA[' +
        '甲<乙&😀' * 30000 + ']]></p><input disabled><p>尾部</p></body></html>')
    write_fixture(directory / "html-late-cdata.epub", html)
    html["EPUB/chapter1.html"] = '\ufeff<!DOCTYPE html><html><body><p disabled>有 BOM 的正文</p></body></html>'
    write_fixture(directory / "html-bom.epub", html)
    for kind, name in (("script", "script"), ("script-case", "ScRiPt"), ("script-ns", "x:script"),
                       ("style", "style"), ("style-case", "StYlE"), ("style-ns", "x:style")):
        local = "script" if kind.startswith("script") else "style"
        payload = ('var x="&missing; </scriptx> 甲😀"; if (a<b) f();' if local == "script" else
                   'a::before {content: "&missing; </stylex> 甲😀 if(a<b)";}')
        before_close = '<!DOCTYPE html><html xmlns:x="urn:raw"><head><' + name + '>' + payload
        before_close += ' ' * ((4093 - len(before_close.encode("utf-8"))) % 4096)
        html["EPUB/chapter1.html"] = before_close + '</' + name.swapcase() + '></head><body><p>正文</p></body></html>'
        write_fixture(directory / ("html-raw-" + kind + ".epub"), html)
    html["EPUB/chapter1.html"] = ('<!DOCTYPE html><html><head><script>var x=1;</script>'
        '<style>a {content: "&lt;safe&gt;";}</style><script/><style/></head><body><p disabled>正文</p></body></html>')
    write_fixture(directory / "html-raw-closed.epub", html)
    html["EPUB/chapter1.html"] = ('<!DOCTYPE html><html xmlns:x="urn:raw"><head><input disabled>'
        '<x:script>var x="&missing;"; if(a<b) f();</x:script>'
        '<x:style>a {content:"&missing; if(a<b)";}</x:style></head><body><p>正文</p></body></html>')
    write_fixture(directory / "html-raw-after-repair.epub", html)
    for name in ("script", "style"):
        for location in ("inner", "outer"):
            before = '<!DOCTYPE html><html><head><' + name + '><' + name + '>'
            if location == "outer":
                before += 'prefix</' + name + '>'
            html["EPUB/chapter1.html"] = (before + '&missing; if(a<b)' +
                ('</' + name + '>&missing; if(a<b)' if location == "inner" else '') +
                '</' + name + '></head><body><p>正文</p></body></html>')
            write_fixture(directory / ("html-raw-nested-" + name + "-" + location + ".epub"), html)

    uri = "denied:data:image/svg+xml;base64,"
    for kind, source in (("denied", uri + 'A' * 10000), ("data", 'data:image/svg+xml;base64,' + 'A' * 10000),
                         ("limit", uri + 'A' * (32768 - len(uri))),
                         ("entities", uri + 'A' * (32767 - len(uri)) + '&amp;'),
                         ("oversized", uri + 'A' * (32769 - len(uri))),
                         ("external", 'https://example.invalid/' + 'A' * 10000)):
        html["EPUB/chapter1.html"] = ('<!DOCTYPE html><html><body><p disabled>前文</p><img src="' +
            source + '"/><p>后文</p></body></html>')
        write_fixture(directory / ("html-image-" + kind + ".epub"), html)
    html["EPUB/chapter1.html"] = '<!doctype html><html><body><p disabled>前文</p><img src="' + uri + 'A' * 10000
    write_fixture(directory / "html-image-truncated.epub", html)
    html["EPUB/chapter1.html"] = '<!doctype html><html><body><p title="' + 'A' * 8193 + '">正文</p></body></html>'
    write_fixture(directory / "html-attribute-oversized.epub", html)



    html["EPUB/chapter1.html"] = '<!doctype html [<!ENTITY x "bad">]><html><body>&x;</body></html>'
    write_fixture(directory / "html-internal-dtd.epub", html)
    html["EPUB/chapter1.html"] = '<!doctype html><html><body><p title="' + 'x' * 65537 + '">正文</p></body></html>'
    write_fixture(directory / "html-token-limit.epub", html)
    xhtml5 = fixture_entries()
    xhtml5["EPUB/chapter1.xhtml"] = '<!DOCTYPE html><html xmlns="http://www.w3.org/1999/xhtml"><body><p>严格源树</p></body></html>'
    write_fixture(directory / "xhtml5-strict.epub", xhtml5)
    strict_html = {name.replace("chapter1.xhtml", "chapter1.html"):
                   value.replace("chapter1.xhtml", "chapter1.html") if isinstance(value, str) else value
                   for name, value in xhtml5.items()}
    write_fixture(directory / "xhtml5-strict-html-extension.epub", strict_html)

    xhtml5["EPUB/chapter1.xhtml"] = '<!DOCTYPE html><html><body><p>&unknown;</p></body></html>'
    write_fixture(directory / "xhtml5-unknown-entity.epub", xhtml5)
    inline = fixture_entries()
    inline["EPUB/chapter1.xhtml"] = ('<html><body><p>前文</p><img src="denied:data:image/svg+xml;base64,' +
        'A' * 10000 + '"/><p>后文</p></body></html>')
    write_fixture(directory / "denied-inline-image.epub", inline)
    invalid = fixture_entries()
    invalid["EPUB/package.opf"] = invalid["EPUB/package.opf"].replace('Pico Page 合成位置样本', 'invalid & title')
    write_fixture(directory / "opf-unescaped-amp.epub", invalid)
    for name, text in (("short", "甲" * 8191), ("boundary", "甲" * 8192),
                       ("long", "甲" * 20000), ("spaces", "word " * 2000),
                       ("entities", "&#x7532;" * 1100), ("crlf", "a\r\n" * 3000),
                       ("parts", "甲" * 130000), ("utf16", "\u8080" * 9000)):
        entries = fixture_entries()
        entries["EPUB/chapter1.xhtml"] = ('<html xmlns="http://www.w3.org/1999/xhtml"><body><p>' +
            text + '<em>内联</em>尾部</p><p>后续正文。</p></body></html>')
        if name == "utf16":
            entries["EPUB/chapter1.xhtml"] = entries["EPUB/chapter1.xhtml"].encode("utf-16")
        write_fixture(directory / ("mapping-node-" + name + ".epub"), entries)
    for kind, package in (("empty", "<wrong/>"), ("paired", "<wrong></wrong>"),
                          ("attribute", '<package id="' + 'x' * 8193 + '"/>')):
        invalid = fixture_entries()
        invalid["EPUB/package.opf"] = package
        write_fixture(directory / ("opf-invalid-" + kind + ".epub"), invalid)
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
    titles = fixture_entries()
    titles["EPUB/nav.xhtml"] = titles["EPUB/nav.xhtml"].replace(
        '<li><a href="chapter2.xhtml">第二章</a></li>',
        '<li><a href="chapter2.xhtml">' + '甲' * 16 + '）？</a></li>'
        '<li><a href="chapter2.xhtml">' + '乙' * 60 + '</a></li>'
        '<li><a href="chapter2.xhtml">短标题</a><ol><li><a href="chapter2.xhtml">' +
        '甲' * 17 + '）</a></li></ol></li>'
        '<li><a href="chapter2.xhtml">末尾一</a></li>'
        '<li><a href="chapter2.xhtml">末尾二</a></li>'
        '<li><a href="chapter2.xhtml">最后一项</a></li>')
    write_fixture(directory / "toc-titles.epub", titles)
    ncx = fixture_entries()
    ncx["EPUB/package.opf"] = ncx["EPUB/package.opf"].replace('version="3.0"', 'version="2.0"').replace(
        '<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>',
        '<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>')
    ncx["EPUB/toc.ncx"] = '''<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/"><navMap>
<navPoint><navLabel><text>  第一卷 &amp; 起点  </text></navLabel><content src="chapter1.xhtml"/>
  <navPoint><navLabel><text> 内联&#x20;文字 </text></navLabel><content src="chapter1.xhtml#mixed"/></navPoint>
</navPoint>
<navPoint><navLabel><text>无目标的父项</text></navLabel>
  <navPoint><navLabel><text> 脚注位置 </text></navLabel><content src="chapter1.xhtml#note"/></navPoint>
</navPoint>
<navPoint><navLabel><text>旧标题</text><text> 第二章 </text></navLabel>
  <content src="chapter1.xhtml"/><content src="chapter2.xhtml#cross-chapter"/>
</navPoint></navMap></ncx>'''
    write_fixture(directory / "ncx-nested.epub", ncx)
    broken_ncx = dict(ncx)
    broken_ncx["EPUB/toc.ncx"] = '<ncx><navMap><navPoint></navMap></ncx>'
    write_fixture(directory / "ncx-malformed.epub", broken_ncx)
    dual = dict(ncx)
    dual["EPUB/package.opf"] = dual["EPUB/package.opf"].replace('</manifest>',
        '<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/></manifest>')
    dual["EPUB/nav.xhtml"] = '<html><nav><a></html>'
    write_fixture(directory / "nav-malformed-ncx.epub", dual)
    dual["EPUB/nav.xhtml"] = '<html><body><nav type="toc"/></body></html>'
    write_fixture(directory / "nav-empty-ncx.epub", dual)
    for kind, count, label in (("entries", 8193, "章"), ("label", 1, "x" * 32769),
                               ("strings", 1100, "x" * 1000)):
        limited = dict(ncx)
        limited["EPUB/toc.ncx"] = '<ncx><navMap>' + ''.join(
            '<navPoint><navLabel><text>' + label + '</text></navLabel>'
            '<content src="chapter1.xhtml"/></navPoint>' for _ in range(count)) + '</navMap></ncx>'
        write_fixture(directory / ("ncx-budget-" + kind + ".epub"), limited)
    excessive = fixture_entries()
    excessive["EPUB/package.opf"] = excessive["EPUB/package.opf"].replace('</manifest>', ''.join(
        f'<item id="extra{i}" href="chapter1.xhtml" media-type="application/xhtml+xml"/>'
        for i in range(4094)) + '</manifest>')
    write_fixture(directory / "opf-budget-items.epub", excessive)
    excessive = fixture_entries()
    excessive["EPUB/package.opf"] = excessive["EPUB/package.opf"].replace('</manifest>', ''.join(
        f'<item id="extra{i}" href="chapter1.xhtml" media-type="application/xhtml+xml" properties="' +
        'x' * 8000 + '"/>' for i in range(140)) + '</manifest>')
    write_fixture(directory / "opf-budget-strings.epub", excessive)
    large = large_epub_entries(3000)
    assert len(large['EPUB/package.opf'].encode()) > 262144
    write_fixture(directory / "large-toc.epub", large)
    write_fixture(directory / "large-toc-reversed.epub", dict(reversed(list(large.items()))))
    write_fixture(directory / "large-toc-small.epub", large_epub_entries(300))
    for chapters in (300, 3000):
        covered = large_epub_entries(chapters)
        covered['EPUB/package.opf'] = covered['EPUB/package.opf'].replace('</manifest>',
            '<item id="cover" href="cover.png" media-type="image/png" properties="cover-image"/></manifest>')
        covered['EPUB/cover.png'] = bytes.fromhex(
            '89504e470d0a1a0a0000000d494844520000000200000002080600000072b60d24'
            '00000006624b474400ff00ff00ffa0bda7930000001549444154789c63606060f80f'
            '020cff19408881010048c906fa001f0d6c0000000049454e44ae426082')
        write_fixture(directory / f'large-cover-{chapters}.epub', covered)
    indexed = {name: value for name, value in fixture_entries().items()
               if name in ("mimetype", "META-INF/container.xml")}
    paths = ["Chapter.xhtml", "chapter.xhtml", "x" * 480 + "a.xhtml", "x" * 480 + "b.xhtml"]
    indexed["EPUB/package.opf"] = '<package><manifest>' + ''.join(
        f'<item id="c{i}" href="{path}" media-type="application/xhtml+xml"/>'
        for i, path in enumerate(paths)) + '</manifest><spine>' + ''.join(
        f'<itemref idref="c{i}"/>' for i in range(len(paths))) + '</spine></package>'
    for i, path in enumerate(paths):
        indexed["EPUB/" + path] = f'<html><body><p>chapter {i}</p></body></html>'
    write_fixture(directory / "zip-index-paths.epub", dict(reversed(list(indexed.items()))))
    missing_case = dict(indexed)
    missing_case["EPUB/package.opf"] = indexed["EPUB/package.opf"].replace('href="Chapter.xhtml"', 'href="CHAPTER.xhtml"')
    write_fixture(directory / "zip-index-missing-case.epub", missing_case)
    duplicate = directory / "zip-index-duplicate.epub"
    write_fixture(duplicate, indexed)
    with zipfile.ZipFile(duplicate, "a") as archive:
        archive.writestr("EPUB/Chapter.xhtml", indexed["EPUB/Chapter.xhtml"])
    nul_names = dict(indexed)
    nul_names["EPUB/Chapter.xhtml!bad"] = nul_names.pop("EPUB/Chapter.xhtml")
    nul_path = directory / "zip-index-nul.epub"
    write_fixture(nul_path, nul_names)
    nul_path.write_bytes(nul_path.read_bytes().replace(b"EPUB/Chapter.xhtml!bad", b"EPUB/Chapter.xhtml\x00bad"))
    (directory / "malformed.epub").write_bytes(b"This is not a ZIP archive.")
    entries = fixture_entries()
    entries["EPUB/chapter2.xhtml"] = "<html><body>" + "超" * 200001 + "</body></html>"
    write_fixture(directory / "oversized-chapter.epub", entries)
    entries = fixture_entries()
    entries["EPUB/chapter1.xhtml"] = '''<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.1//EN" "http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd"><html xmlns="http://www.w3.org/1999/xhtml">
<head><link rel="stylesheet" href="book.css"/><style>p { margin: 0; font-size: 1.5em; }</style></head>
<body><p style="text-align:justify">正文保留 <em>内联</em><span style="display:none">隐藏</span></p>
<p><img src="image.png" alt="示例图片" style="width:40%;max-width:80px"/></p></body></html>'''
    entries["EPUB/book.css"] = "p { font-size: 1em; }"
    write_fixture(directory / "styled.epub", entries)
    font_layout = dict(entries)
    font_layout["EPUB/chapter1.xhtml"] = entries["EPUB/chapter1.xhtml"].replace(
        "隐藏</span></p>", '隐藏</span> 开花一页<span style="font-family:Example OpenType">甲</span>。</p>')
    font_layout["EPUB/Fonts/welcome.ttf"] = (Path(__file__).resolve().parents[1] /
        "firmware/components/reader_graphics/fonts/PicoPageWelcome.ttf").read_bytes()
    font_layout["EPUB/Fonts/other.otf"] = (Path(__file__).resolve().parents[1] /
        "firmware/components/reader_graphics/fonts/PicoPageFallback.otf").read_bytes()
    font_layout["EPUB/book.css"] = (
        "p { font-size: 1em; font-family: 'Example Face', serif; }"
        "@font-face { font-family: Example Face; src: url('missing.ttf'), url(Fonts/welcome.ttf); }"
        "@font-face { font-family: 'Example OpenType'; src: url('Fonts/other.otf'); }")
    font_layout["EPUB/package.opf"] = entries["EPUB/package.opf"].replace(
        "</manifest>", '<item id="embedded-font" href="Fonts/welcome.ttf" media-type="font/ttf"/>'
        '<item id="other-font" href="Fonts/other.otf" media-type="font/otf"/></manifest>')
    write_fixture(directory / "font-layout.epub", font_layout)
    mapping = dict(entries)
    mapping["EPUB/chapter1.xhtml"] = mapping["EPUB/chapter1.xhtml"].replace(
        '<span style="display:none">隐藏</span>', "").replace(
        '<p><img src="image.png" alt="示例图片" style="width:40%;max-width:80px"/></p>', '<p> \n<em>起点</em> 尾部</p>')
    write_fixture(directory / "mapping.epub", mapping)
    entries["EPUB/chapter1.xhtml"] = entries["EPUB/chapter1.xhtml"].replace("正文保留", "正文&missing;")
    write_fixture(directory / "unresolved-entity.epub", entries)
    entries = fixture_entries()
    entries["EPUB/chapter1.xhtml"] = '<html xmlns="http://www.w3.org/1999/xhtml"><body><img src="cover.png" alt="封面"/></body></html>'
    entries["EPUB/cover.png"] = bytes.fromhex("89504e470d0a1a0a0000000d494844520000000200000002080600000072b60d2400000006624b474400ff00ff00ffa0bda7930000001549444154789c63606060f80f020cff19408881010048c906fa001f0d6c0000000049454e44ae426082")
    entries["EPUB/large.png"] = b"\x89PNG\r\n\x1a\n" + bytes(range(256)) * 8192
    write_fixture(directory / "image-cover.epub", entries)
    gif_cover = dict(entries)
    gif_cover["EPUB/chapter1.xhtml"] = entries["EPUB/chapter1.xhtml"].replace("cover.png", "cover.gif")
    del gif_cover["EPUB/cover.png"]
    gif_cover["EPUB/cover.gif"] = bytes.fromhex("47494638396101000100800000000000ffffff2c00000000010001000002024401003b")
    write_fixture(directory / "gif-image-cover.epub", gif_cover)
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
    boundary = dict(entries)
    boundary["EPUB/chapter1.xhtml"] = ('<html xmlns="http://www.w3.org/1999/xhtml"><body>' + "".join(
        f"<p>{number:04d}这一段文字用于把第一个分段正好填满到分段上限为止。</p>" for number in range(2000)) +
        '<img src="cover.png" alt="分段首图"/><p>图片之后的正文。</p></body></html>')
    write_fixture(directory / "part-boundary-image.epub", boundary)
    hidden = dict(entries)
    hidden["EPUB/chapter1.xhtml"] = ('<html xmlns="http://www.w3.org/1999/xhtml"><body><p>隐藏图片之前的正文。</p>'
                                     '<img src="cover.png" alt="隐藏图片" style="display:none"/><p>隐藏图片之后的正文。</p></body></html>')
    hidden["EPUB/chapter2.xhtml"] = ('<html xmlns="http://www.w3.org/1999/xhtml"><body><p>块级图片之前的正文。</p>'
                                     '<img src="cover.png" alt="块级图片" style="display: block"/><p>块级图片之后的正文。</p></body></html>')
    write_fixture(directory / "hidden-image.epub", hidden)
    table = fixture_entries()
    table["EPUB/chapter1.xhtml"] = ('<html xmlns="http://www.w3.org/1999/xhtml"><body><p>表格之前的正文。</p>'
                                    '<table><tr><td>单元格</td></tr></table><p>表格之后的正文。</p></body></html>')
    write_fixture(directory / "table-chapter.epub", table)
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

    streaming = fixture_entries()
    streaming["EPUB/chapter1.xhtml"] = ('<html><body><p>' +
        ('甲😀 &amp; &#x4e59;\r\n' * 30000) + '</p>' + ''.join(
        f'<p id="s{i}"><a href="#s0">{hashlib.sha256(str(i).encode()).hexdigest()}</a>'
        '<em>尾部</em><br/>' + ('<img src="missing.png" alt="图"/>' if i % 16 == 0 else '') +
        '</p>' for i in range(4500)) + '</body></html>')
    write_fixture(directory / "stream-events.epub", streaming)
    with zipfile.ZipFile(directory / "stream-stored.epub", "w", compression=zipfile.ZIP_STORED) as archive:
        for name, content in streaming.items():
            archive.writestr(name, content)
    malformed = dict(streaming)
    malformed["EPUB/chapter1.xhtml"] = malformed["EPUB/chapter1.xhtml"].replace("</body></html>", "</broken>")
    write_fixture(directory / "stream-malformed.epub", malformed)
    deferred = dict(streaming)
    element = 's' * 1000
    deferred["EPUB/chapter1.xhtml"] = ('<html><!--' + 'x' * 40000 + '--><body><' + element + '>' +
        '<p>甲</p>' * 800 + '</' + element + '></body></html>')
    write_fixture(directory / "stream-deferred-final.epub", deferred)
    excessive = dict(streaming)
    excessive["EPUB/chapter1.xhtml"] = ('<html><!--' + 'x' * (1024 * 1024) + '--><body>' +
        '<p>连续解析保留正文与源位置。</p>' * 12000 + '</body></html>')
    write_fixture(directory / "stream-large-token.epub", excessive)
    damaged = bytearray((directory / "stream-events.epub").read_bytes())
    at = 0
    while True:
        at = damaged.find(b"PK\x01\x02", at)
        if at < 0:
            raise AssertionError("missing chapter directory entry")
        name_size = struct.unpack_from("<H", damaged, at + 28)[0]
        if damaged[at + 46:at + 46 + name_size] == b"EPUB/chapter1.xhtml":
            damaged[at + 16] ^= 1
            break
        at += 46 + name_size
    (directory / "stream-bad-crc.epub").write_bytes(damaged)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    generate(parser.parse_args().directory)
