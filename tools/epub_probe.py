"""Inspect one EPUB spine resource without creating a reading package or XPointer."""

import argparse
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import posixpath
import re
import stat
import sys
from urllib.parse import unquote, urlsplit
import xml.etree.ElementTree as ET
import zipfile
import zlib

MAX_ARCHIVE = 16 * 1024 * 1024
MAX_ENTRY = 1024 * 1024
MAX_TOTAL = 8 * 1024 * 1024
MAX_ENTRIES = 128
MAX_NODES = 10000
MAX_DEPTH = 64
MAX_SCALARS = 200000
CONTAINER = "urn:oasis:names:tc:opendocument:xmlns:container"
OPF = "http://www.idpf.org/2007/opf"
XHTML = "http://www.w3.org/1999/xhtml"
EPUB = "http://www.idpf.org/2007/ops"
SPACE = " \t\r\n"
BLOCKS = {"body", "div", "section", "article", "aside", "p", "h1", "h2", "h3",
          "h4", "h5", "h6", "ul", "ol", "li", "blockquote", "nav"}
INLINE = {"span", "em", "strong", "b", "i", "a", "sup", "sub", "br", "hr", "img"}


class EpubError(ValueError):
    """The file is malformed, unsafe, over budget or outside the supported subset."""


class BoundedTree(ET.TreeBuilder):
    def __init__(self):
        super().__init__()
        self.depth = 0
        self.nodes = 0

    def start(self, tag, attrs):
        self.depth += 1
        self.nodes += 1
        if self.depth > MAX_DEPTH or self.nodes > MAX_NODES:
            raise EpubError("XML depth/node budget exceeded")
        return super().start(tag, attrs)

    def end(self, tag):
        self.depth -= 1
        return super().end(tag)

    def doctype(self, name, pubid, system):
        raise EpubError("XML DTD is unsupported")


def partial_md5(data: bytes) -> str:
    """Return KOReader's sampled digest; this is not a strong content identity."""
    digest = hashlib.md5(usedforsecurity=False)
    # LuaJIT's 32-bit lshift(1024, -2) yields zero for the first sample.
    for offset in [0] + [1024 * 4 ** i for i in range(11)]:
        sample = data[offset:offset + 1024]
        if not sample:
            break
        digest.update(sample)
    return digest.hexdigest()


def resource_path(base: str, href: str) -> str:
    """Resolve a local EPUB URI and reject archive escapes and external resources."""
    parsed = urlsplit(href)
    path = unquote(parsed.path, errors="strict")
    if parsed.scheme or parsed.netloc or parsed.query or path.startswith("/") or "\\" in path or "\x00" in path:
        raise EpubError("unsupported or unsafe resource URI")
    resolved = posixpath.normpath(posixpath.join(posixpath.dirname(base), path)) if path else base
    if resolved == ".." or resolved.startswith("../") or resolved in {"", "."}:
        raise EpubError("resource URI escapes archive")
    return resolved


def read_entry(archive: zipfile.ZipFile, name: str) -> bytes:
    try:
        with archive.open(name) as stream:
            data = stream.read(MAX_ENTRY + 1)
    except (KeyError, zipfile.BadZipFile, RuntimeError, OSError, zlib.error) as error:
        raise EpubError(f"missing resource or invalid ZIP entry: {name}") from error
    if len(data) > MAX_ENTRY:
        raise EpubError("ZIP entry size budget exceeded")
    return data


def read_xml(archive: zipfile.ZipFile, name: str) -> ET.Element:
    try:
        text = read_entry(archive, name).decode("utf-8-sig")
        declaration = re.match(r"\s*<\?xml[^?]*encoding=['\"]([^'\"]+)", text)
        if declaration and declaration[1].lower() not in {"utf-8", "utf8"}:
            raise EpubError("XML encoding is unsupported; UTF-8 is required")
        return ET.fromstring(text, parser=ET.XMLParser(target=BoundedTree()))
    except (UnicodeError, ET.ParseError) as error:
        raise EpubError(f"invalid UTF-8 XML: {name}") from error


def validate_archive(archive: zipfile.ZipFile) -> None:
    entries = archive.infolist()
    if len(entries) > MAX_ENTRIES or sum(item.file_size for item in entries) > MAX_TOTAL:
        raise EpubError("ZIP entry count/total size budget exceeded")
    seen = set()
    for item in entries:
        name = item.filename.rstrip("/")
        if (not name or name.startswith("/") or "\\" in name or "\x00" in item.orig_filename
                or ".." in PurePosixPath(name).parts or posixpath.normpath(name) != name):
            raise EpubError("unsafe ZIP path")
        if name in seen:
            raise EpubError("duplicate ZIP path")
        seen.add(name)
        if item.file_size > MAX_ENTRY:
            raise EpubError("ZIP entry size budget exceeded")
        if item.flag_bits & 1 or item.compress_type not in {zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED}:
            raise EpubError("encrypted or unsupported ZIP compression")
        if stat.S_ISLNK(item.external_attr >> 16):
            raise EpubError("ZIP symbolic links are unsupported")
        if item.file_size > max(item.compress_size, 1) * 200:
            raise EpubError("ZIP compression ratio budget exceeded")
    if not entries or entries[0].filename != "mimetype" or entries[0].compress_type != zipfile.ZIP_STORED:
        raise EpubError("EPUB mimetype must be the first uncompressed entry")
    if read_entry(archive, "mimetype") != b"application/epub+zip":
        raise EpubError("invalid EPUB mimetype")
    if "META-INF/encryption.xml" in seen:
        raise EpubError("encrypted/obfuscated resources are unsupported")


def chapter_text(archive: zipfile.ZipFile, href: str) -> dict:
    root = read_xml(archive, href)
    if root.tag != f"{{{XHTML}}}html":
        raise EpubError("chapter requires XHTML html root")
    body = root.find(f"{{{XHTML}}}body")
    if body is None or len(root.findall(f"{{{XHTML}}}body")) != 1:
        raise EpubError("chapter requires exactly one XHTML body")
    if any(root.findall(f".//{{{XHTML}}}{tag}") for tag in ("style", "link", "script")):
        raise EpubError("unsupported styling: stylesheets are not interpreted")
    for element in root.iter():
        if any(key in element.attrib for key in ("style", "hidden", "{http://www.w3.org/XML/1998/namespace}space")):
            raise EpubError("unsupported styling or whitespace override")
    text, origins, nodes, images = [], [], {}, []
    limitations = set()
    scalar_count = 0

    def append(character, origin):
        if len(text) >= MAX_SCALARS:
            raise EpubError("chapter scalar budget exceeded")
        text.append(character)
        origins.append(origin)

    def boundary():
        if text and text[-1] == " ":
            text.pop()
            origins.pop()
        if text and text[-1] != "\n":
            append("\n", None)

    def content(value, node):
        nonlocal scalar_count
        if not value:
            return
        scalar_count += len(value)
        if scalar_count > MAX_SCALARS:
            raise EpubError("chapter scalar budget exceeded")
        nodes[node] = value
        for offset, character in enumerate(value):
            if character in SPACE:
                if not text or text[-1] in " \n":
                    continue
                character = " "
            append(character, (node, offset))

    def visit(element, path):
        if not element.tag.startswith(f"{{{XHTML}}}"):
            raise EpubError("unsupported element namespace")
        tag = element.tag.split("}", 1)[1]
        if tag not in BLOCKS | INLINE:
            raise EpubError(f"unsupported element: {tag}")
        if "footnote" in element.get(f"{{{EPUB}}}type", "").split():
            limitations.add("footnotes_in_document_order")
        if tag in BLOCKS or tag in {"br", "hr"}:
            boundary()
        if tag == "img":
            if not element.get("src"):
                raise EpubError("image requires a resource URI")
            target = resource_path(href, element.get("src"))
            read_entry(archive, target)
            images.append({"resource": target, "alt": element.get("alt", "")})
            limitations.add("images_not_rendered")
        content(element.text, path + "#text")
        counts = {}
        for child in element:
            name = child.tag.split("}")[-1]
            counts[name] = counts.get(name, 0) + 1
            child_path = f"{path}/{name}[{counts[name]}]"
            visit(child, child_path)
            content(child.tail, child_path + "#tail")
        if tag in BLOCKS:
            boundary()

    visit(body, "/html[1]/body[1]")
    while text and text[-1] in " \n":
        text.pop()
        origins.pop()
    runs = []
    for index, origin in enumerate(origins):
        node, offset = origin if origin else (None, None)
        if runs and node == runs[-1]["node"] and offset is not None and offset == runs[-1]["source_start"] + index - runs[-1]["start"]:
            runs[-1]["end"] = index + 1
        else:
            runs.append({"start": index, "end": index + 1, "node": node, "source_start": offset})
    return {"resource": href, "text": "".join(text), "source_nodes": nodes, "source_map": runs,
            "images": images, "limitations": sorted(limitations)}


def source_at(chapter: dict, scalar_offset: int) -> dict | None:
    """Map an output scalar to decoded XML text; generated block breaks return None."""
    if not 0 <= scalar_offset < len(chapter["text"]):
        raise EpubError("chapter scalar offset out of range")
    for run in chapter["source_map"]:
        if run["start"] <= scalar_offset < run["end"]:
            if run["node"] is None:
                return None
            return {"node": run["node"], "scalar_offset": run["source_start"] + scalar_offset - run["start"]}
    raise EpubError("missing source mapping")


def probe(path: Path, spine_index: int = 0) -> dict:
    """Read a bounded snapshot, identify the original ZIP and parse one spine item."""
    with path.open("rb") as source:
        data = source.read(MAX_ARCHIVE + 1)
    if len(data) > MAX_ARCHIVE:
        raise EpubError("archive size budget exceeded")
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            validate_archive(archive)
            container = read_xml(archive, "META-INF/container.xml")
            roots = container.findall(f"{{{CONTAINER}}}rootfiles/{{{CONTAINER}}}rootfile")
            if container.tag != f"{{{CONTAINER}}}container" or len(roots) != 1:
                raise EpubError("exactly one EPUB package is supported")
            package_path = resource_path("", roots[0].get("full-path", ""))
            package = read_xml(archive, package_path)
            if package.tag != f"{{{OPF}}}package":
                raise EpubError("invalid OPF package root")
            manifest = {}
            for item in package.findall(f"{{{OPF}}}manifest/{{{OPF}}}item"):
                item_id = item.get("id")
                if not item_id or item_id in manifest:
                    raise EpubError("missing or duplicate manifest id")
                manifest[item_id] = item
            spine = []
            for ref in package.findall(f"{{{OPF}}}spine/{{{OPF}}}itemref"):
                item = manifest.get(ref.get("idref"))
                if item is None or item.get("media-type") != "application/xhtml+xml":
                    raise EpubError("spine requires a manifest XHTML resource")
                if ref.get("linear", "yes") != "yes":
                    raise EpubError("nonlinear spine items are unsupported")
                spine.append(resource_path(package_path, item.get("href", "")))
            if not 0 <= spine_index < len(spine):
                raise EpubError("spine index out of range")
            chapter = chapter_text(archive, spine[spine_index])
    except (zipfile.BadZipFile, NotImplementedError) as error:
        raise EpubError("invalid or unsupported ZIP") from error
    return {"probe_version": 1, "normalization": "xml-space-collapse-block-newline-v1",
            "offset_unit": "unicode_scalar", "identity": {"sha256": hashlib.sha256(data).hexdigest(),
            "kosync_partial_md5": partial_md5(data)}, "spine": spine, "spine_index": spine_index,
            "chapter": chapter, "kosync": {"identity_status": "source_algorithm_only",
            "position_status": "not_verified"}}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("epub", type=Path)
    parser.add_argument("--spine", type=int, default=0, help="zero-based chapter index")
    args = parser.parse_args()
    try:
        print(json.dumps(probe(args.epub, args.spine), ensure_ascii=False, indent=2))
    except (EpubError, OSError, ValueError) as error:
        print(f"EPUB probe failed: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
