#!/usr/bin/env python3
"""Receive a bounded firmware tar stream through a forced SSH command."""
import fcntl
import io
import json
import os
from pathlib import Path
import re
import shutil
import sys
import tarfile
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ota_manifest

ORIGIN = 'https://oc.wulu.zone/firmware/'
ROOT = Path('/srv/pico-page/firmware')
NAME = re.compile(r'manifest\.json|pico-page-' + ota_manifest.RELEASE.pattern + r'-read_pico-release(?:-full)?\.bin')


def publish(stream, root=ROOT):
    payload = stream.read(9 * 1024 * 1024 + 1)
    if len(payload) > 9 * 1024 * 1024:
        raise ValueError('Firmware archive exceeds 9 MiB')
    with (root / '.deploy.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        with tempfile.TemporaryDirectory(prefix='.deploy-', dir=root) as temporary:
            stage = Path(temporary)
            received = set()
            with tarfile.open(fileobj=io.BytesIO(payload), mode='r|') as archive:
                for member in archive:
                    limit = 64 * 1024 if member.name == 'manifest.json' else ota_manifest.APP_OFFSET + ota_manifest.SLOT_BYTES
                    if (not member.isfile() or not NAME.fullmatch(member.name) or member.name in received
                            or len(received) >= 3 or not 0 < member.size <= limit):
                        raise ValueError('Archive contains an invalid firmware entry')
                    received.add(member.name)
                    with archive.extractfile(member) as source, (stage / member.name).open('xb') as target:
                        shutil.copyfileobj(source, target)
            version = ota_manifest.check_directory(stage, ORIGIN)
            if not version:
                raise ValueError('Firmware manifest is required')
            manifest = json.loads((stage / 'manifest.json').read_text())
            expected = {'manifest.json', manifest['url'].rsplit('/', 1)[-1]}
            full = ota_manifest.full_name(version)
            if full in received:
                expected.add(full)
            if received != expected:
                raise ValueError('Archive contains files outside the manifest')
            for name in sorted(received - {'manifest.json'}):
                destination = root / name
                if destination.is_symlink() or (destination.exists() and destination.read_bytes() != (stage / name).read_bytes()):
                    raise ValueError('Published firmware cannot be overwritten: ' + name)
            directory = os.open(root, os.O_RDONLY)
            try:
                for names in (sorted(received - {'manifest.json'}), ['manifest.json']):
                    for name in names:
                        source = stage / name
                        source.chmod(0o640)
                        with source.open('rb') as handle:
                            os.fsync(handle.fileno())
                        source.replace(root / name)
                    os.fsync(directory)
            finally:
                os.close(directory)
            print('Published firmware ' + version)


if __name__ == '__main__':
    if os.environ.get('SSH_ORIGINAL_COMMAND') != 'publish':
        sys.exit('Only the publish command is permitted')
    try:
        os.umask(0o027)
        publish(sys.stdin.buffer)
    except (OSError, ValueError, tarfile.TarError) as error:
        sys.exit(str(error))
