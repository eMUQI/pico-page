import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from ci_changes import checks_for_paths

SCRIPT = Path(__file__).resolve().parents[1] / 'tools/ci_changes.py'


class CiChangesTest(unittest.TestCase):
    def test_check_selection(self):
        cases = [
            ([], False, False),
            (['README.md', 'docs/development-status.md', 'artifacts/screen.png'], False, False),
            (['docs/releases/0.2.0.md'], False, True),
            (['web/src/pages/index.astro', '.github/workflows/web.yml'], False, True),
            (['firmware/components/reader_app/app.cpp', 'docs/development-status.md'], True, False),
            (['tools/host/main.cpp', 'tests/app_test.cpp', 'CMakeLists.txt'], True, False),
            (['docs/examples/global-settings.toml'], False, False),
            (['.github/workflows/host.yml', 'tools/check_handoff.py'], True, False),
            (['tools/ota_manifest.py'], True, True),
            (['tools/deploy_firmware.py'], True, True),
            (['tools/publish_cos.py', 'tools/cos-requirements.txt'], True, True),
            (['tools/ci_changes.py'], True, True),
            (['firmware/partitions.build.csv'], True, True),
            (['firmware/board/profile.cmake'], True, True),
            (['new-component/source.cpp', 'docs/releases/0.2.0.md'], True, True),
        ]
        for paths, host, web in cases:
            with self.subTest(paths=paths):
                self.assertEqual(checks_for_paths(paths), {'host': host, 'web': web})

    def test_manual_run_selects_all_checks_without_git(self):
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.check_output([sys.executable, str(SCRIPT)], cwd=directory, text=True)
        self.assertEqual(result, 'host=true\nweb=true\n')

    def test_git_diffs_cover_pr_paths_and_deployment_successors(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)

            def git(*args):
                return subprocess.check_output(['git', *args], cwd=root, text=True).strip()

            git('init', '-q')
            git('config', 'user.email', 'test@example.invalid')
            git('config', 'user.name', 'CI Test')
            git('config', 'commit.gpgsign', 'false')
            (root / 'source.cpp').write_text('source\n')
            git('add', '.')
            git('commit', '-qm', 'Base')
            base = git('rev-parse', 'HEAD')
            (root / 'web').mkdir()
            os.rename(root / 'source.cpp', root / 'web' / 'name\nwith newline.js')
            git('add', '-A')
            git('commit', '-qm', 'Move source')
            (root / 'README.md').write_text('Documentation\n')
            git('add', '.')
            git('commit', '-qm', 'Documentation only tip')
            result = subprocess.check_output(
                [sys.executable, str(SCRIPT), '--base', base], cwd=root, text=True)
            self.assertEqual(result, 'host=true\nweb=true\n')
            result = subprocess.run(
                [sys.executable, str(SCRIPT), '--base', 'missing-base'], cwd=root, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, b'')
            site_build = git('rev-parse', 'HEAD')
            (root / 'README.md').write_text('Updated documentation\n')
            git('add', '.')
            git('commit', '-qm', 'Unrelated main update')
            docs_only = git('rev-parse', 'HEAD')
            release_notes = root / 'docs/releases/0.2.0.md'
            release_notes.parent.mkdir(parents=True)
            release_notes.write_text('Next release notes\n')
            git('add', '.')
            git('commit', '-qm', 'Prepare release notes')
            notes_only = git('rev-parse', 'HEAD')
            result = subprocess.check_output(
                [sys.executable, str(SCRIPT), '--base', docs_only], cwd=root, text=True)
            self.assertEqual(result, 'host=false\nweb=true\n')
            (root / 'web' / 'name\nwith newline.js').write_text('Updated website\n')
            git('add', '.')
            git('commit', '-qm', 'Newer website inputs')
            newer_site = git('rev-parse', 'HEAD')
            git('checkout', '--detach', '-q', site_build)
            for successor, expected in [(docs_only, 'false'), (notes_only, 'true'), (newer_site, 'true')]:
                result = subprocess.check_output(
                    [sys.executable, str(SCRIPT), '--compare', successor], cwd=root, text=True)
                self.assertEqual(result, f'host=false\nweb={expected}\n')
            result = subprocess.run(
                [sys.executable, str(SCRIPT), '--compare', 'missing-main'], cwd=root, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, b'')


if __name__ == '__main__':
    unittest.main()
