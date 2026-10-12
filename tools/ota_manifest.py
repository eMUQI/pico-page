#!/usr/bin/env python3
"""Write the firmware update manifest and the published image name for one release build.

The device reads the manifest from the address compiled into the firmware and downloads the
image the manifest names. Nothing here uploads or publishes; the output directory is copied to
the web host separately.
"""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
BOARD = 'read_pico'
PROJECT = 'pico_page'
SLOT_BYTES = 4 * 1024 * 1024
NOTES_LIMIT = 8 * 1024
ESP32S3_CHIP_ID = 9
RELEASE = re.compile(r'(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)(?:-(?:beta|rc)\.[1-9][0-9]*)?')
# esp_image_header_t (24 bytes), esp_image_segment_header_t (8 bytes), then esp_app_desc_t.
DESCRIPTION = 32


def schema_defaults(root=ROOT):
    """The data format versions this source tree reads and writes, from the device's own constants."""
    header = (root / 'firmware/components/reader_sync/include/pico_page/update.h').read_text()
    found = re.search(r'progress_schema = (\d+), config_schema = (\d+);', header)
    if not found:
        raise ValueError('update.h does not declare progress_schema and config_schema')
    return int(found.group(1)), int(found.group(2))


def image_version(image):
    """Version from the application description of an ESP32-S3 pico_page image."""
    if len(image) < DESCRIPTION + 256 or image[0] != 0xE9:
        raise ValueError('Not an ESP application image')
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
    if not base_url.startswith('https://') or not base_url.endswith('/'):
        raise ValueError('Base URL must start with https:// and end with /')
    datetime.date.fromisoformat(published)
    name = f'pico-page-{version}-{BOARD}-release.bin'
    return name, dict(manifest=1, board=BOARD, version=version, published_at=published, url=base_url + name,
                      size=len(image), sha256=hashlib.sha256(image).hexdigest(),
                      data_schema=dict(progress=schema[0], config=schema[1]), notes=notes)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True, help='release build of pico_page.bin')
    parser.add_argument('--notes', type=Path, required=True, help='Markdown release notes, UTF-8')
    parser.add_argument('--base-url', default='https://picopage.wulu.zone/firmware/')
    parser.add_argument('--published', default=datetime.date.today().isoformat(), help='YYYY-MM-DD')
    parser.add_argument('--out', type=Path, required=True, help='directory for manifest.json and the named image')
    arguments = parser.parse_args()
    try:
        name, manifest = build_manifest(arguments.image.read_bytes(), arguments.notes.read_text(encoding='utf-8'),
                                        arguments.base_url, arguments.published, schema_defaults())
    except (OSError, ValueError) as error:
        sys.exit(str(error))
    arguments.out.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(arguments.image, arguments.out / name)
    (arguments.out / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(arguments.out / 'manifest.json')


if __name__ == '__main__':
    main()
