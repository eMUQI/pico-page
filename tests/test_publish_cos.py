"""Verify that COS publication preserves immutable images and does not bypass a closed bucket."""
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
from urllib.error import HTTPError, URLError

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import ota_manifest
import publish_cos
from test_ota_manifest import image, table, LAYOUT


class ObjectExists(Exception):
    def get_error_code(self):
        return 'FileAlreadyExists'


class ObjectMissing(Exception):
    def get_error_code(self):
        return 'NoSuchKey'


class CosFixture:
    def __init__(self):
        self.files = {}
        self.puts = []

    def put_object(self, **request):
        self.puts.append(request)
        if request['Key'] in self.files:
            raise ObjectExists()
        self.files[request['Key']] = request['Body'].read()

    def get_object(self, **request):
        if request['Key'] not in self.files:
            raise ObjectMissing()
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
            self.assertEqual(len(cos.puts), 2)
            for request in cos.puts:
                self.assertNotIn('ACL', request)
                self.assertEqual(request['Metadata'], {'x-cos-forbid-overwrite': 'true'})
                self.assertTrue(request['Body'].closed)
            saved = dict(cos.files)
            self.prepare(root, image(size=8192))
            with self.assertRaisesRegex(ValueError, 'differs'):
                publish_cos.publish(cos, root)
            self.assertEqual(cos.files, saved)
            self.assertEqual(len(cos.puts), 2)
            with patch.object(cos, 'get_object', side_effect=RuntimeError('read failed')):
                with self.assertRaisesRegex(RuntimeError, 'read failed'):
                    publish_cos.publish(cos, root)
            self.assertEqual(len(cos.puts), 2)

    def test_invalid_release_is_rejected_before_upload(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.prepare(root)
            (root / 'extra.bin').write_bytes(b'unrelated')
            cos = CosFixture()
            with self.assertRaises(ValueError):
                publish_cos.publish(cos, root)
            self.assertEqual(cos.puts, [])

    def test_public_verification_requires_identical_bytes_and_limits_403_fallback(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.prepare(root)
            files = {p.name: p.read_bytes() for p in root.iterdir()}
            requests = []
            responses = {}

            def fetch(request, timeout):
                url = request.full_url
                requests.append(url)
                value = responses.get(url, files[url.rsplit('/', 1)[-1]])
                if isinstance(value, Exception):
                    if isinstance(value, HTTPError):
                        self.addCleanup(value.close)
                    raise value
                stream = io.BytesIO(value)
                stream.status = 200
                return stream

            with patch.object(publish_cos.urllib.request, 'urlopen', side_effect=fetch):
                self.assertEqual(publish_cos.verify_public(root), 'public')
                self.assertTrue(all(url.startswith(publish_cos.ORIGIN) for url in requests))
                first = publish_cos.ORIGIN + sorted(root.glob('*.bin'))[0].name
                for error in [HTTPError(first, 404, 'Missing', {}, io.BytesIO()),
                              HTTPError(first, 500, 'Unavailable', {}, io.BytesIO()), URLError('timeout')]:
                    requests.clear()
                    responses[first] = error
                    with self.assertRaises(type(error)):
                        publish_cos.verify_public(root)
                    self.assertEqual(requests, [first])
                responses[first] = b'changed COS bytes'
                with self.assertRaisesRegex(ValueError, 'differs'):
                    publish_cos.verify_public(root)
                responses[first] = HTTPError(first, 403, 'Denied', {}, io.BytesIO())
                self.assertEqual(publish_cos.verify_public(root), 'unchanged')
                for name in files:
                    url = publish_cos.SITE + name
                    for value in [b'different production bytes',
                                  HTTPError(url, 403, 'Denied', {}, io.BytesIO()), URLError('timeout')]:
                        responses[url] = value
                        with self.subTest(file=name, failure=type(value).__name__):
                            with self.assertRaises((ValueError, HTTPError, URLError)):
                                publish_cos.verify_public(root)
                    del responses[url]
                (root / ota_manifest.full_name('0.2.0-beta.2')).unlink()
                for path in root.glob('*.bin'):
                    url = publish_cos.ORIGIN + path.name
                    responses[url] = HTTPError(url, 403, 'Denied', {}, io.BytesIO())
                with self.assertRaisesRegex(ValueError, 'full-flash image'):
                    publish_cos.verify_public(root)


if __name__ == '__main__':
    unittest.main()
