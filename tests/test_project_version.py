from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import json

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from project_version import project_version
import firmware_provenance


class ProjectVersionTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.git('init', '-q')
        self.git('config', 'user.email', 'test@example.invalid')
        self.git('config', 'user.name', 'Version Test')
        self.git('config', 'commit.gpgsign', 'false')
        (self.root / 'firmware').mkdir()
        self.version = self.root / 'firmware/version.txt'
        self.version.write_text('0.2.0-beta.1\n')
        self.git('add', '.')
        self.git('commit', '-qm', 'Fixture')

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.root), *args], text=True).strip()

    def test_development_dirty_and_tagged_release(self):
        self.assertEqual(project_version(self.root), '0.2.0-beta.1+g' + self.git('rev-parse', 'HEAD')[:7])
        with self.assertRaises(ValueError):
            project_version(self.root, require_release=True)
        self.git('-c', 'tag.gpgsign=false', 'tag', 'v0.2.0-beta.1')
        self.assertEqual(project_version(self.root, require_release=True), '0.2.0-beta.1')
        (self.root / 'untracked').write_text('changed')
        self.assertTrue(project_version(self.root).endswith('.dirty'))
        with self.assertRaises(ValueError):
            project_version(self.root, require_release=True)

    def test_mismatched_tag(self):
        self.git('-c', 'tag.gpgsign=false', 'tag', 'v0.2.0-beta.2')
        with self.assertRaises(ValueError):
            project_version(self.root, require_release=True)

    def test_syntax_and_descriptor_limit(self):
        for value in ('0.2.0', '1.0.0-rc.1', '0.2.0-beta.10'):
            self.version.write_text(value)
            self.assertTrue(project_version(self.root).startswith(value + '+g'))
        for value in ('v0.2.0', '0.02.0', '0.2.0-beta.01', '0.2.0-beta.0',
                      '0.2.0\n1.0.0', '0.2.0+manual', '123456789.123456789.123456789-beta.1'):
            self.version.write_text(value)
            with self.assertRaises(ValueError):
                project_version(self.root)

    def test_provenance_rejects_stale_descriptor(self):
        build = self.root / 'build/firmware-release'
        build.mkdir(parents=True)
        (build / 'source-before.json').write_text('{}')
        project = 'pico_page_ota7'
        application = build / f'{project}.bin'
        elf = build / f'{project}.elf'
        (build / 'project_description.json').write_text(json.dumps({
            'project_name': project, 'app_bin': application.name, 'app_elf': elf.name,
        }))
        elf.write_bytes(b'elf')
        (build / 'sdkconfig').write_text('')
        image = bytearray(112)
        image[32:36] = bytes.fromhex('3254cdab')
        version = b'0.2.0-beta.1'
        image[48:48 + len(version)] = version
        image[80:80 + len(project)] = project.encode()
        application.write_bytes(image)
        (build / 'pico_page.bin').write_bytes(b'stale old build')
        with patch.object(firmware_provenance, 'ROOT', self.root), \
             patch.object(firmware_provenance, 'source_state', return_value={}), \
             patch.object(firmware_provenance, 'require_defaults'), \
             patch.object(firmware_provenance, 'project_version', return_value=version.decode()), \
             patch.object(sys, 'argv', ['firmware_provenance.py', 'finish', str(build)]):
            firmware_provenance.main()
            manifest = json.loads((build / 'provenance.json').read_text())
            self.assertEqual(manifest['version'], version.decode())
            self.assertEqual(manifest['build_mode'], 'release')
            self.assertEqual(set(manifest['artifacts_sha256']), {application.name, elf.name, 'sdkconfig'})
            wrong_project = bytearray(image)
            wrong_project[80:112] = b'pico_page'.ljust(32, b'\0')
            application.write_bytes(wrong_project)
            with self.assertRaises(RuntimeError):
                firmware_provenance.main()
            image[48] = ord('9')
            application.write_bytes(image)
            with self.assertRaises(RuntimeError):
                firmware_provenance.main()


if __name__ == '__main__':
    unittest.main()
