"""Verify that COS publication preserves immutable images and does not bypass a closed bucket."""
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import ota_manifest
import publish_cos
from test_ota_manifest import image, table, LAYOUT


class ObjectExists(Exception):
    def get_error_code(self):
        return 'ObjectAlreadyExists'


class CosFixture:
    def __init__(self):
        self.files = {}
        self.puts = []

    def put_object(self, **request):
        self.puts.append(request)
        if request['Key'] in self.files:
            raise ObjectExists()
        self.files[request['Key']] = request['Body']

    def get_object(self, **request):
        class Body:
            def get_raw_stream(self):
                return io.BytesIO(self.data)
        body = Body()
        body.data = self.files[request['Key']]
        return {'Body': body}


class PublishCosTests(unittest.TestCase):
    def prepare(self, root, data=None):
        data = image() if data is None else data
        name, manifest = ota_manifest.build_manifest(data, '', publish_cos.ORIGIN, '2026-10-18', (1, 1))
        (root / name).write_bytes(data)
        (root / 'manifest.json').write_text(json.dumps(manifest))
        (root / ota_manifest.full_name(manifest['version'])).write_bytes(
            ota_manifest.build_full(image(), table(*LAYOUT), data))

    def test_upload_and_retry_leave_acl_inherited_and_images_immutable(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.prepare(root)
            cos = CosFixture()
            for _ in range(2):
                self.assertEqual(publish_cos.publish(cos, root), '0.2.0-beta.2')
            self.assertEqual(set(cos.files), {p.name for p in root.glob('*.bin')})
            for request in cos.puts:
                self.assertNotIn('ACL', request)
                self.assertEqual(request['Metadata'], {'x-cos-forbid-overwrite': 'true'})
            saved = dict(cos.files)
            self.prepare(root, image(size=8192))
            with self.assertRaisesRegex(ValueError, 'differs'):
                publish_cos.publish(cos, root)
            self.assertEqual(cos.files, saved)

    def test_invalid_release_is_rejected_before_upload(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.prepare(root)
            (root / 'extra.bin').write_bytes(b'unrelated')
            cos = CosFixture()
            with self.assertRaises(ValueError):
                publish_cos.publish(cos, root)
            self.assertEqual(cos.puts, [])


if __name__ == '__main__':
    unittest.main()
