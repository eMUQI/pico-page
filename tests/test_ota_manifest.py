import hashlib
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import ota_manifest


def image(version='0.2.0-beta.2', project='pico_page', chip=9, size=4096):
    data = bytearray(size)
    data[0] = 0xE9
    struct.pack_into('<H', data, 12, chip)
    struct.pack_into('<I', data, 32, 0xABCD5432)
    data[48:48 + len(version)] = version.encode()
    data[80:80 + len(project)] = project.encode()
    return bytes(data)


class ManifestTest(unittest.TestCase):
    def build(self, data, notes='## 新增\n- 书签', url='https://picopage.wulu.zone/firmware/'):
        return ota_manifest.build_manifest(data, notes, url, '2026-10-18', (1, 1))

    def test_fields_describe_the_image(self):
        data = image()
        name, manifest = self.build(data)
        self.assertEqual(name, 'pico-page-0.2.0-beta.2-read_pico-release.bin')
        self.assertEqual(manifest, dict(
            manifest=1, board='read_pico', version='0.2.0-beta.2', published_at='2026-10-18',
            url='https://picopage.wulu.zone/firmware/' + name, size=len(data),
            sha256=hashlib.sha256(data).hexdigest(), data_schema=dict(progress=1, config=1), notes='## 新增\n- 书签'))

    def test_rejects_what_the_device_would_refuse(self):
        for data in (image('0.2.0-beta.2+g1234567'), image(project='other'), image(chip=5), b'\xe9' * 64,
                     image(size=ota_manifest.SLOT_BYTES + 1)):
            with self.assertRaises(ValueError):
                self.build(data)
        with self.assertRaises(ValueError):
            self.build(image(), notes='长' * 3000)
        for url in ('http://picopage.wulu.zone/firmware/', 'https://picopage.wulu.zone/firmware'):
            with self.assertRaises(ValueError):
                self.build(image(), url=url)
        with self.assertRaises(ValueError):
            ota_manifest.build_manifest(image(), '', 'https://picopage.wulu.zone/firmware/', '18 Oct', (1, 1))

    def test_check_accepts_only_a_manifest_that_describes_its_image(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / 'firmware'
            url = 'https://picopage.wulu.zone/firmware/'
            self.assertIsNone(ota_manifest.check_directory(directory, url))
            directory.mkdir()
            self.assertIsNone(ota_manifest.check_directory(directory, url))
            data = image()
            name, manifest = self.build(data)
            (directory / name).write_bytes(data)
            with self.assertRaises(ValueError):
                ota_manifest.check_directory(directory, url)
            (directory / 'manifest.json').write_text(json.dumps(manifest), encoding='utf-8')
            self.assertEqual(ota_manifest.check_directory(directory, url), '0.2.0-beta.2')
            # Every field is recomputed from the image, so a stale digest, size or version is caught.
            for key, value in (('sha256', '0' * 64), ('size', 1), ('version', '0.2.0-beta.3'), ('board', 'other'),
                               ('url', 'https://example.com/firmware/' + name), ('extra', 1)):
                (directory / 'manifest.json').write_text(json.dumps({**manifest, key: value}), encoding='utf-8')
                with self.assertRaises(ValueError):
                    ota_manifest.check_directory(directory, url)
            (directory / 'manifest.json').write_text(json.dumps(manifest), encoding='utf-8')
            (directory / 'pico-page-0.2.0-beta.1-read_pico-release.bin').write_bytes(image('0.2.0-beta.1'))
            with self.assertRaises(ValueError):
                ota_manifest.check_directory(directory, url)
            (directory / name).unlink()
            with self.assertRaises(ValueError):
                ota_manifest.check_directory(directory, url)

    def test_schema_defaults_follow_the_device_constants(self):
        self.assertEqual(ota_manifest.schema_defaults(), (1, 1))


if __name__ == '__main__':
    unittest.main()
