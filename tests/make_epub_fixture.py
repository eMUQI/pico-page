"""Generate an original, deterministic EPUB for local parser and KOReader probes."""

import argparse
from pathlib import Path
import zipfile


def fixture_entries(variant: str = "甲") -> dict[str, str]:
    return {
        "mimetype": "application/epub+zip",
        "META-INF/container.xml": '''<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
<rootfiles><rootfile full-path="EPUB/package.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>''',
        "EPUB/package.opf": '''<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
<dc:identifier id="id">urn:uuid:7db7c8bf-016c-4d3f-a5e7-94b47a856037</dc:identifier>
<dc:title>Pico Page 合成位置样本</dc:title><dc:language>zh-CN</dc:language>
<meta property="dcterms:modified">2026-10-05T00:00:00Z</meta></metadata>
<manifest><item id="one" href="chapter1.xhtml" media-type="application/xhtml+xml"/>
<item id="two" href="chapter2.xhtml" media-type="application/xhtml+xml"/>
<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/></manifest>
<spine><itemref idref="one"/><itemref idref="two"/></spine></package>''',
        "EPUB/nav.xhtml": '''<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
<head><title>目录</title></head><body><nav epub:type="toc"><ol>
<li><a href="chapter1.xhtml">第一章</a></li><li><a href="chapter2.xhtml">第二章</a></li>
</ol></nav></body></html>''',
        "EPUB/chapter1.xhtml": f'''<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
<head><title>第一章</title></head><body><h1>第一章：春水</h1>
<p id="mixed">  中文 <em>内联𠮷😀</em>\t 与 English &amp; 空白。&#160;保留不换行空格。</p>
<p>脚注入口<a epub:type="noteref" href="#note">一</a>，内容版本{variant}。</p>
<ul><li>列表第一项</li><li>列表<strong>第二项</strong>。</li></ul>
<aside epub:type="footnote" id="note"><p>这是原创脚注文本。</p></aside>
</body></html>''',
        "EPUB/chapter2.xhtml": '''<html xmlns="http://www.w3.org/1999/xhtml">
<head><title>第二章</title></head><body><h1>第二章：远山</h1>
<p id="cross-chapter">跨章位置应由资源和源节点确定，不由页码确定。</p>
<p>''' + "长段落包含中文与 ABC，检查连续内容。" * 96 + '''</p>
</body></html>''',
    }


def write_fixture(path: Path, entries: dict[str, str | bytes] | None = None) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w") as archive:
        for name, text in (entries if entries is not None else fixture_entries()).items():
            info = zipfile.ZipInfo(name, date_time=(2026, 10, 5, 0, 0, 0))
            info.compress_type = zipfile.ZIP_STORED if name == "mimetype" else zipfile.ZIP_DEFLATED
            archive.writestr(info, text.encode("utf-8") if isinstance(text, str) else text)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    write_fixture(args.output)
    print(args.output)
