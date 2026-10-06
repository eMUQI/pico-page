"""Focused checks for source offsets, identity and untrusted EPUB boundaries."""

import hashlib
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from epub_probe import EpubError, probe, source_at, partial_md5
from make_epub_fixture import fixture_entries, write_fixture


class EpubProbeTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.book = Path(self.temp.name) / "same-name.epub"

    def test_real_zip_spine_and_scalar_source_mapping(self):
        write_fixture(self.book)
        first = probe(self.book, 0)
        chapter = first["chapter"]
        self.assertEqual(first["spine"], ["EPUB/chapter1.xhtml", "EPUB/chapter2.xhtml"])
        self.assertIn("中文 内联𠮷😀 与 English & 空白。\u00a0保留", chapter["text"])
        point = source_at(chapter, chapter["text"].index("😀"))
        self.assertEqual(point["scalar_offset"], 3)
        self.assertTrue(point["node"].endswith("/em[1]#text"))
        self.assertEqual(chapter["source_nodes"][point["node"]][point["scalar_offset"]], "😀")
        for offset, character in enumerate(chapter["text"]):
            origin = source_at(chapter, offset)
            if origin is None:
                self.assertEqual(character, "\n")
            else:
                original = chapter["source_nodes"][origin["node"]][origin["scalar_offset"]]
                self.assertEqual(character, " " if original in " \t\r\n" else original)
        self.assertIn("列表第二项。", chapter["text"])
        self.assertIn("footnotes_in_document_order", chapter["limitations"])
        self.assertEqual(first["kosync"]["position_status"], "not_verified")
        self.assertNotIn("xpointer", first["kosync"])
        second = probe(self.book, 1)
        self.assertIn("跨章位置", second["chapter"]["text"])
        self.assertEqual(first["identity"], second["identity"])

    def test_content_identity_and_partial_sampling(self):
        write_fixture(self.book)
        original = probe(self.book)["identity"]
        write_fixture(self.book, fixture_entries("乙"))
        self.assertNotEqual(original["sha256"], probe(self.book)["identity"]["sha256"])
        data = bytes(range(251)) * 300
        expected = hashlib.md5(data[:1024] + data[1024:2048] + data[4096:5120]
                               + data[16384:17408] + data[65536:66560]).hexdigest()
        self.assertEqual(partial_md5(data), expected)

    def test_current_chapter_only_and_explicit_unsupported_content(self):
        entries = fixture_entries()
        entries["EPUB/chapter2.xhtml"] = "<malformed"
        write_fixture(self.book, entries)
        self.assertIn("第一章", probe(self.book, 0)["chapter"]["text"])
        with self.assertRaisesRegex(EpubError, "XML"):
            probe(self.book, 1)
        for fragment, expected in [("<img src='missing.png'/>", "missing resource"),
                                   ("<table><tr><td>文字</td></tr></table>", "unsupported element"),
                                   ("<p style='display:none'>文字</p>", "unsupported styling")]:
            with self.subTest(fragment=fragment):
                entries = fixture_entries()
                entries["EPUB/chapter1.xhtml"] = entries["EPUB/chapter1.xhtml"].replace("</body>", fragment + "</body>")
                write_fixture(self.book, entries)
                with self.assertRaisesRegex(EpubError, expected):
                    probe(self.book)

    def test_rejects_unsafe_or_unbounded_input(self):
        cases = []
        entries = fixture_entries()
        entries["../outside"] = "x"
        cases.append((entries, "unsafe ZIP path"))
        dtd = "<!DOCTYPE x [<!ENTITY e SYSTEM 'file:///etc/passwd'>]><x>&e;</x>"
        for value, expected in [(dtd, "DTD"), (dtd.encode("utf-16"), "UTF-8 XML")]:
            entries = fixture_entries()
            entries["EPUB/chapter1.xhtml"] = value
            cases.append((entries, expected))
        entries = fixture_entries()
        entries["EPUB/chapter1.xhtml"] = "<x>" * 70 + "</x>" * 70
        cases.append((entries, "depth"))
        entries = fixture_entries()
        entries["large.bin"] = "x" * (1024 * 1024 + 1)
        cases.append((entries, "entry size"))
        for href, expected in [("../../outside.xhtml", "escapes archive"),
                               ("https://example.invalid/chapter.xhtml", "unsafe resource URI")]:
            entries = fixture_entries()
            entries["EPUB/package.opf"] = entries["EPUB/package.opf"].replace('href="chapter1.xhtml"', f'href="{href}"')
            cases.append((entries, expected))
        for entries, expected in cases:
            with self.subTest(expected=expected):
                write_fixture(self.book, entries)
                with self.assertRaisesRegex(EpubError, expected):
                    probe(self.book)


if __name__ == "__main__":
    unittest.main()
