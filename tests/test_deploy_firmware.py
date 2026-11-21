import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import deploy_firmware
import ota_manifest
from test_ota_manifest import image, table, LAYOUT, OTA7_LAYOUT


def archive(entries):
    payload = io.BytesIO()
    with tarfile.open(fileobj=payload, mode='w') as output:
        for name, data, kind in entries:
            member = tarfile.TarInfo(name)
            member.size, member.type = len(data), kind
            output.addfile(member, io.BytesIO(data))
    payload.seek(0)
    return payload


class DeployFirmwareTest(unittest.TestCase):
    def entries(self, data=None, layout=LAYOUT):
        data = image() if data is None else data
        name, manifest = ota_manifest.build_manifest(data, '', deploy_firmware.ORIGIN, '2026-10-18', (1, 1))
        full = ota_manifest.build_full(image(), table(*layout), data)
        return [(name, data, tarfile.REGTYPE),
                (ota_manifest.full_name(manifest['version']), full, tarfile.REGTYPE),
                ('manifest.json', json.dumps(manifest).encode(), tarfile.REGTYPE)]

    def test_publish_retry_and_reject_changed_existing_version(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            entries = self.entries()
            for _ in range(2):
                deploy_firmware.publish(archive(entries), root)
                for name, data, _ in entries:
                    self.assertEqual((root / name).read_bytes(), data)
                    self.assertEqual((root / name).stat().st_mode & 0o777, 0o640)
            changed = self.entries(image(size=8192))
            with self.assertRaisesRegex(ValueError, 'cannot be overwritten'):
                deploy_firmware.publish(archive(changed), root)
            for name, data, _ in entries:
                self.assertEqual((root / name).read_bytes(), data)

    def test_bad_archives_preserve_the_published_manifest(self):
        entries = self.entries()
        broken = [(entries[0][0], entries[0][1][:-1], tarfile.REGTYPE)] + entries[1:]
        cases = [[], entries[:-1], broken, entries + [entries[0]],
                 [('../escape', b'bad', tarfile.REGTYPE)],
                 [('manifest.json', b'', tarfile.SYMTYPE)],
                 [('manifest.json', b'x' * (64 * 1024 + 1), tarfile.REGTYPE)]]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'manifest.json').write_bytes(b'previous')
            for items in cases:
                with self.subTest(names=[entry[0] for entry in items]), self.assertRaises((ValueError, tarfile.TarError)):
                    deploy_firmware.publish(archive(items), root)
                self.assertEqual((root / 'manifest.json').read_bytes(), b'previous')
                self.assertEqual({p.name for p in root.iterdir()}, {'manifest.json', '.deploy.lock'})
            with self.assertRaisesRegex(ValueError, 'exceeds'):
                deploy_firmware.publish(io.BytesIO(b'x' * (9 * 1024 * 1024 + 1)), root)

            ota7 = self.entries(image(version='0.7.0', project='pico_page_ota7'), OTA7_LAYOUT)
            with self.assertRaisesRegex(ValueError, 'Expected legacy firmware layout'):
                deploy_firmware.publish(archive(ota7), root)
            self.assertEqual((root / 'manifest.json').read_bytes(), b'previous')


if __name__ == '__main__':
    unittest.main()
