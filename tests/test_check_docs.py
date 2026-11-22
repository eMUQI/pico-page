import contextlib
import io
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import check_docs


class DocumentationCheckTest(unittest.TestCase):
    def test_public_tree_and_broken_links(self):
        source = check_docs.ROOT
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            for relative in ('AGENTS.md', 'docs/examples/global-settings.toml',
                             'docs/examples/book-settings.toml',
                             'firmware/components/reader_graphics/fonts',
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
                ('[规则](AGENTS.md)\n', 0),
                ('[缺失文档](missing.md)\n', 1),
                ('![缺失画面](missing.png)\n', 1),
                ('完整记录保存在归档仓库。\n', 1),
            )
            with patch.object(check_docs, 'ROOT', root):
                for text, expected in cases:
                    with self.subTest(text=text):
                        guide.write_text(text, encoding='utf-8')
                        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                            self.assertEqual(check_docs.main(), expected)
                guide.unlink()
                user_guide = root / 'docs/user/runtime.md'
                user_guide.parent.mkdir(parents=True)
                for text, expected in (('正在快速迭代。\n', 0), ('固件仍处于测试阶段。\n', 1), ('当前正式版为 0.6.0。\n', 1)):
                    with self.subTest(text=text):
                        user_guide.write_text(text, encoding='utf-8')
                        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                            self.assertEqual(check_docs.main(), expected)


if __name__ == '__main__':
    unittest.main()
