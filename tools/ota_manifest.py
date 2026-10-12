#!/usr/bin/env python3
"""Write or check the firmware update manifest and the published image for one release build.

The device reads the manifest from the address compiled into the firmware and downloads the
image the manifest names. Both are attached to the GitHub release of a version; the website
build copies those of the newest release into web/public/firmware, which is not kept in the
repository. Nothing here uploads or publishes.
"""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import urllib.parse

ROOT = Path(__file__).resolve().parents[1]
BOARD = 'read_pico'
PROJECT = 'pico_page'
SLOT_BYTES = 4 * 1024 * 1024
NOTES_LIMIT = 8 * 1024
ESP32S3_CHIP_ID = 9
# The device reads version numbers of at most nine digits.
NUMBER = r'(?:0|[1-9][0-9]{0,8})'
RELEASE = re.compile(rf'{NUMBER}\.{NUMBER}\.{NUMBER}(?:-(?:beta|rc)\.[1-9][0-9]{{0,8}})?')
# esp_image_header_t (24 bytes), esp_image_segment_header_t (8 bytes), then esp_app_desc_t.
DESCRIPTION = 32


def schema_defaults(root=ROOT):
    """The data format versions this source tree reads and writes, from the device's own constants."""
    header = (root / 'firmware/components/reader_sync/include/pico_page/update.h').read_text()
    found = re.search(r'progress_schema = (\d+), config_schema = (\d+);', header)
    if not found:
        raise ValueError('update.h does not declare progress_schema and config_schema')
    return int(found.group(1)), int(found.group(2))


def verify_image(image):
    """Check the whole image as the device's bootloader does: segments, checksum, appended digest.

    An image that keeps a valid start but is truncated or altered would otherwise be published
    and fail on every device only after its complete download.
    """
    if len(image) < DESCRIPTION + 256 or image[0] != 0xE9:
        raise ValueError('Not an ESP application image')
    offset, checksum = 24, 0xEF
    for _ in range(image[1]):
        length, = struct.unpack_from('<I', image, offset + 4)
        offset += 8
        if offset + length > len(image):
            raise ValueError('Image is truncated')
        for word in struct.iter_unpack('<I', image[offset:offset + length - length % 4]):
            checksum ^= word[0] ^ word[0] >> 8 ^ word[0] >> 16 ^ word[0] >> 24
        for byte in image[offset + length - length % 4:offset + length]:
            checksum ^= byte
        offset += length
    # Zero padding places the checksum in the last byte of a 16-byte block.
    offset = (offset | 15) + 1
    if offset > len(image) or image[offset - 1] != checksum & 0xFF:
        raise ValueError('Image checksum does not match')
    if image[23] != 1:
        raise ValueError('Image has no appended SHA-256')
    if image[offset:] != hashlib.sha256(image[:offset]).digest():
        raise ValueError('Image digest does not match its contents')


def image_version(image):
    """Version from the application description of a complete ESP32-S3 pico_page image."""
    verify_image(image)
    if struct.unpack_from('<H', image, 12)[0] != ESP32S3_CHIP_ID:
        raise ValueError('Image is not built for ESP32-S3')
    magic, = struct.unpack_from('<I', image, DESCRIPTION)
    version, project = (image[DESCRIPTION + offset:DESCRIPTION + offset + 32].split(b'\0')[0].decode()
                        for offset in (16, 48))
    if magic != 0xABCD5432 or project != PROJECT:
        raise ValueError('Image is not a ' + PROJECT + ' application')
    return version


def build_manifest(image, notes, base_url, published, schema):
    version = image_version(image)
    if not RELEASE.fullmatch(version):
        raise ValueError('Image version ' + version + ' is not a release version; build from a clean tagged checkout')
    if len(image) > SLOT_BYTES:
        raise ValueError('Image exceeds the 4 MiB application slot')
    if len(notes.encode()) > NOTES_LIMIT:
        raise ValueError('Release notes exceed 8 KiB; the device would cut them')
    datetime.date.fromisoformat(published)
    name = f'pico-page-{version}-{BOARD}-release.bin'
    # The device accepts only such addresses: https with a host, printable ASCII without user
    # information, query, fragment or backslash, at most 512 bytes.
    parts = urllib.parse.urlsplit(base_url)
    if (parts.scheme != 'https' or not parts.hostname or '@' in parts.netloc or parts.query or parts.fragment
            or not parts.path.endswith('/') or not base_url.startswith('https://' + parts.netloc)
            or any(not '!' <= c <= '~' or c in '\\#?' for c in base_url) or len(base_url + name) > 512):
        raise ValueError('Base URL must be a plain https:// address that ends with /')
    return name, dict(manifest=1, board=BOARD, version=version, published_at=published, url=base_url + name,
                      size=len(image), sha256=hashlib.sha256(image).hexdigest(),
                      data_schema=dict(progress=schema[0], config=schema[1]), notes=notes)


def newest_release(releases):
    """Tag of the highest released version among `gh release list --json tagName,isDraft` entries.

    Only tags of the form v<release version> count. Order follows docs/versioning.md:
    X.Y.Z-beta.N before X.Y.Z-rc.N before X.Y.Z. None when no such release exists.
    """
    def order(tag):
        core, _, stage = tag[1:].partition('-')
        name, _, number = stage.partition('.')
        return [int(part) for part in core.split('.')] + [('beta', 'rc', '').index(name), int(number or 0)]
    tags = [entry['tagName'] for entry in releases
            if not entry.get('isDraft') and entry['tagName'][:1] == 'v' and RELEASE.fullmatch(entry['tagName'][1:])]
    return max(tags, key=order, default=None)


def check_directory(directory, base_url):
    """Verify that a published directory holds one manifest that describes its one image.

    Returns the published version, or None when the directory publishes nothing.
    """
    images = sorted(path.name for path in directory.glob('*.bin')) if directory.is_dir() else []
    path = directory / 'manifest.json'
    if not path.is_file():
        if images:
            raise ValueError('Images without manifest.json: ' + ', '.join(images))
        return None
    manifest = json.loads(path.read_text(encoding='utf-8'))
    schema = manifest.get('data_schema') if isinstance(manifest, dict) else None
    if (not isinstance(schema, dict) or not all(type(schema.get(key)) is int and schema[key] >= 1 for key in ('progress', 'config'))
            or not all(isinstance(manifest.get(key), str) for key in ('url', 'notes', 'published_at'))):
        raise ValueError('manifest.json lacks url, notes, published_at or data_schema')
    name = manifest['url'].rsplit('/', 1)[-1]
    if not (directory / name).is_file():
        raise ValueError('manifest.json names ' + name + ', which is not in ' + str(directory))
    expected = build_manifest((directory / name).read_bytes(), manifest['notes'], base_url, manifest['published_at'],
                              (schema['progress'], schema['config']))[1]
    if manifest != expected:
        differing = sorted(key for key in set(manifest) | set(expected) if manifest.get(key) != expected.get(key))
        raise ValueError('manifest.json does not describe ' + name + ': ' + ', '.join(differing))
    if images != [name]:
        raise ValueError('Images that the manifest does not name: ' + ', '.join(sorted(set(images) - {name})))
    return manifest['version']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', type=Path, metavar='DIR', help='verify a published directory instead of writing one')
    parser.add_argument('--expect-version', help='with --check: the version that must be published')
    parser.add_argument('--pick-release', type=Path, metavar='JSON',
                        help='print the newest release tag from the output of gh release list --json tagName,isDraft')
    parser.add_argument('--image', type=Path, help='release build of pico_page.bin')
    parser.add_argument('--notes', type=Path, help='Markdown release notes, UTF-8')
    parser.add_argument('--base-url', default='https://picopage.wulu.zone/firmware/')
    parser.add_argument('--published', default=datetime.date.today().isoformat(), help='YYYY-MM-DD')
    parser.add_argument('--out', type=Path, help='directory for manifest.json and the named image')
    arguments = parser.parse_args()
    if arguments.pick_release:
        print(newest_release(json.loads(arguments.pick_release.read_text(encoding='utf-8'))) or '')
        return
    if arguments.check:
        try:
            version = check_directory(arguments.check, arguments.base_url)
        except (OSError, ValueError) as error:
            sys.exit(str(error))
        if arguments.expect_version and version != arguments.expect_version:
            sys.exit('Expected firmware ' + arguments.expect_version + ', found ' + (version or 'none'))
        print('Firmware ' + version + ' matches its manifest' if version else 'No firmware published')
        return
    if not (arguments.image and arguments.notes and arguments.out):
        parser.error('--image, --notes and --out are required unless --check is given')
    try:
        image = arguments.image.read_bytes()
        name, manifest = build_manifest(image, arguments.notes.read_text(encoding='utf-8'),
                                        arguments.base_url, arguments.published, schema_defaults())
    except (OSError, ValueError) as error:
        sys.exit(str(error))
    arguments.out.mkdir(parents=True, exist_ok=True)
    # The manifest names one image; an earlier release in the directory would only be dead weight.
    for stale in arguments.out.glob('pico-page-*-' + BOARD + '-release.bin'):
        stale.unlink()
    (arguments.out / name).write_bytes(image)
    (arguments.out / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(arguments.out / 'manifest.json')


if __name__ == '__main__':
    main()
