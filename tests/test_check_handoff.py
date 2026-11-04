import contextlib
import io
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import check_handoff


class DocumentationCheckTest(unittest.TestCase):
    def test_public_tree_and_broken_links(self):
        source = check_handoff.ROOT
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            for relative in ('AGENTS.md', 'docs/design/inventory.json',
                             'docs/design/original/png', 'examples/sdcard/PicoPage',
                             'examples/sidecar', 'firmware/components/reader_graphics/fonts',
                             'web/public/downloads/LXGWWenKai-v1.520-OFL.txt'):
                origin, destination = source / relative, root / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                if origin.is_dir():
                    shutil.copytree(origin, destination)
                else:
                    shutil.copyfile(origin, destination)
            (root / 'AGENTS.md').write_text(
                '所有可交互页面 K2 = ForceRefresh\nNVS 不保存隐藏的权威阅读配置/进度\n',
                encoding='utf-8')
            guide = root / 'guide.md'
            cases = (
                ('[规则](AGENTS.md)\n![画面](docs/design/original/png/S02-1.png)\n', 0),
                ('[缺失文档](missing.md)\n', 1),
                ('![缺失画面](missing.png)\n', 1),
            )
            with patch.object(check_handoff, 'ROOT', root):
                for text, expected in cases:
                    with self.subTest(text=text):
                        guide.write_text(text, encoding='utf-8')
                        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                            self.assertEqual(check_handoff.main(), expected)


if __name__ == '__main__':
    unittest.main()
