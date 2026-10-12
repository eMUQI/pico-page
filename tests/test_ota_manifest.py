import functools
import hashlib
import json
import operator
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import ota_manifest


def image(version='0.2.0-beta.2', project='pico_page', chip=9, size=4096):
    """A complete image: header, one segment that starts with the application description, checksum and digest."""
    body = bytearray(size)
    struct.pack_into('<I', body, 0, 0xABCD5432)
    body[16:16 + len(version)] = version.encode()
    body[48:48 + len(project)] = project.encode()
    header = bytearray(24)
    header[0], header[1], header[23] = 0xE9, 1, 1
    struct.pack_into('<H', header, 12, chip)
    data = bytes(header) + struct.pack('<II', 0x3C000020, len(body)) + bytes(body)
    data += bytes((len(data) | 15) - len(data)) + bytes([functools.reduce(operator.xor, body, 0xEF)])
    return data + hashlib.sha256(data).digest()


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
        for data in (image('0.2.0-beta.2+g1234567'), image('1234567890.0.0'), image(project='other'), image(chip=5), b'\xe9' * 64,
                     image(size=ota_manifest.SLOT_BYTES + 1)):
            with self.assertRaises(ValueError):
                self.build(data)
        # A valid start is not enough: every device would reject these only after a full download.
        whole = image()
        for data in (whole[:-1], whole[:2000], whole + b'\0', whole[:300] + bytes([whole[300] ^ 1]) + whole[301:],
                     whole[:23] + b'\0' + whole[24:], whole[:1] + b'\x11' + whole[2:],
                     whole[:1] + b'\x10' + whole[2:32] + bytes(256)):
            with self.assertRaises(ValueError):
                self.build(data)
        with self.assertRaises(ValueError):
            self.build(image(), notes='长' * 3000)
        # The device refuses these addresses, so a manifest must never be written with them.
        for url in ('http://picopage.wulu.zone/firmware/', 'https://picopage.wulu.zone/firmware', 'https:///firmware/',
                    'https://user@picopage.wulu.zone/firmware/', 'https://picopage.wulu.zone/a b/', 'https://#/',
                    'https://picopage.wulu.zone/firmware/#x/', 'https://picopage.wulu.zone/' + 'a' * 500 + '/',
                    'https://?origin/', 'https://picopage.wulu.zone/firmware/?v=1/', 'https://:443/firmware/',
                    'https://picopage.wulu.zone\\firmware/'):
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

    def test_newest_release_follows_version_order(self):
        tags = ['v0.2.0-beta.2', 'v0.2.0-beta.10', 'v0.2.0-rc.1', 'v0.2.0', 'v0.2.1-beta.1', 'v0.10.0-beta.1', 'v0.9.9']
        releases = [dict(tagName=tag, isDraft=False) for tag in tags]
        self.assertEqual(ota_manifest.newest_release(releases), 'v0.10.0-beta.1')
        self.assertEqual(ota_manifest.newest_release(releases[:4]), 'v0.2.0')
        self.assertEqual(ota_manifest.newest_release(releases[:3]), 'v0.2.0-rc.1')
        self.assertEqual(ota_manifest.newest_release(releases[:2]), 'v0.2.0-beta.10')
        # Drafts and tags outside the version scheme are never published to devices.
        self.assertIsNone(ota_manifest.newest_release(
            [dict(tagName='v0.1.0-mvp', isDraft=False), dict(tagName='v0.3.0', isDraft=True),
             dict(tagName='0.3.0', isDraft=False), dict(tagName='nightly', isDraft=False)]))
        self.assertIsNone(ota_manifest.newest_release([]))

    def test_schema_defaults_follow_the_device_constants(self):
        self.assertEqual(ota_manifest.schema_defaults(), (2, 1))


if __name__ == '__main__':
    unittest.main()
