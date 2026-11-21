#!/usr/bin/env python3
"""Write or check the firmware update manifest and the published images for one release build.

The device reads the manifest from the address compiled into the firmware and downloads the
application image the manifest names. The website installer writes the full-flash image, which
holds the bootloader, the partition table and the same application, to an erased flash from
address 0. The files are attached to the GitHub release of a version; the website build copies
those of the newest release into web/public/firmware, which is not kept in the repository.
Nothing here uploads or publishes.

With --check --publish-base-url, the verified deployment copy can use a different HTTPS
download origin without rebuilding images or changing the original release attachments.
"""
import argparse
import csv
from dataclasses import dataclass
import datetime
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import urllib.parse

from release_notes import TRANSLATED_LANGUAGES, validate_notes

ROOT = Path(__file__).resolve().parents[1]
BOARD = 'read_pico'
FLASH_BYTES = 16 * 1024 * 1024
TABLE_OFFSET = 0x8000
TABLE_BYTES = 0x1000
APP_OFFSET = 0x10000
MANIFEST_LIMIT = 32 * 1024
LOCALIZED_MANIFEST_LIMIT = 23 * 1024
ESP32S3_CHIP_ID = 9
NUMBER = r'(?:0|[1-9][0-9]{0,8})'
RELEASE = re.compile(rf'{NUMBER}\.{NUMBER}\.{NUMBER}(?:-(?:beta|rc)\.[1-9][0-9]{{0,8}})?')
# esp_image_header_t (24 bytes), esp_image_segment_header_t (8 bytes), then esp_app_desc_t.
DESCRIPTION = 32


@dataclass(frozen=True)
class Layout:
    name: str
    project: str
    manifest_format: int
    slot_bytes: int


LEGACY = Layout('legacy', 'pico_page', 1, 4 * 1024 * 1024)
OTA7 = Layout('ota7', 'pico_page_ota7', 2, 7 * 1024 * 1024)
APPLICATION_LAYOUTS = {layout.project: layout for layout in (LEGACY, OTA7)}


def schema_defaults(root=ROOT):
    header = (root / 'firmware/components/reader_sync/include/pico_page/update.h').read_text()
    found = re.search(r'progress_schema = (\d+), config_schema = (\d+);', header)
    if not found:
        raise ValueError('update.h does not declare progress_schema and config_schema')
    return int(found.group(1)), int(found.group(2))


def verify_image(image):
    if len(image) < DESCRIPTION + 256 or image[0] != 0xE9:
        raise ValueError('Not an ESP image')
    # ESP_IMAGE_MAX_SEGMENTS: the bootloader refuses an image with more segments.
    if image[1] > 16:
        raise ValueError('Image has more than 16 segments')
    offset, checksum = 24, 0xEF
    for _ in range(image[1]):
        if offset + 8 > len(image):
            raise ValueError('Image is truncated')
        length, = struct.unpack_from('<I', image, offset + 4)
        offset += 8
        if offset + length > len(image):
            raise ValueError('Image is truncated')
        if length % 4:
            raise ValueError('Image segment length is not a multiple of 4')
        for word in struct.iter_unpack('<I', image[offset:offset + length]):
            checksum ^= word[0] ^ word[0] >> 8 ^ word[0] >> 16 ^ word[0] >> 24
        offset += length
    # Zero padding places the checksum in the last byte of a 16-byte block.
    offset = (offset | 15) + 1
    if offset > len(image) or image[offset - 1] != checksum & 0xFF:
        raise ValueError('Image checksum does not match')
    if image[23] != 1:
        raise ValueError('Image has no appended SHA-256')
    if image[offset:offset + 32] != hashlib.sha256(image[:offset]).digest():
        raise ValueError('Image digest does not match its contents')
    return offset + 32


def image_details(image):
    if verify_image(image) != len(image):
        raise ValueError('Image does not end with its digest')
    if struct.unpack_from('<H', image, 12)[0] != ESP32S3_CHIP_ID:
        raise ValueError('Image is not built for ESP32-S3')
    # ESP-IDF stores the application descriptor at the start of the first segment.
    if struct.unpack_from('<I', image, 28)[0] < 256:
        raise ValueError('First segment is shorter than the application description')
    magic, = struct.unpack_from('<I', image, DESCRIPTION)
    version, project = (image[DESCRIPTION + offset:DESCRIPTION + offset + 32].split(b'\0')[0].decode()
                        for offset in (16, 48))
    layout = APPLICATION_LAYOUTS.get(project)
    if magic != 0xABCD5432 or layout is None:
        raise ValueError('Image is not a recognized Pico Page application')
    return version, layout


def image_version(image):
    return image_details(image)[0]


def download_url(base_url, filename):
    url = base_url + filename
    parts = urllib.parse.urlsplit(base_url)
    if (parts.scheme != 'https' or not parts.hostname or '@' in base_url or parts.query or parts.fragment
            or not parts.path.endswith('/') or not base_url.startswith('https://' + parts.netloc)
            or any(not '!' <= c <= '~' or c in '\\#?' for c in base_url) or len(url) > 512):
        raise ValueError('Base URL must be a plain https:// address that ends with /')
    return url


def serialize_manifest(manifest):
    payload = json.dumps(manifest, ensure_ascii=False, indent=2) + '\n'
    size = len(payload.encode('utf-8'))
    if size > MANIFEST_LIMIT:
        raise ValueError('Manifest exceeds the 32 KiB HTTP response limit')
    if 'notes_i18n' in manifest and size > LOCALIZED_MANIFEST_LIMIT:
        raise ValueError('Localized manifest exceeds 23 KiB, leaving 1 KiB for cached state metadata')
    return payload


def build_manifest(image, notes, base_url, published, schema, *, notes_i18n=None):
    version, layout = image_details(image)
    if not RELEASE.fullmatch(version):
        raise ValueError('Image version ' + version + ' is not a release version; build from a clean tagged checkout')
    if len(image) > layout.slot_bytes:
        raise ValueError(f'Image exceeds the {layout.slot_bytes // (1024 * 1024)} MiB application slot')
    validate_notes(notes, 'zh-Hans', required=notes_i18n is not None)
    datetime.date.fromisoformat(published)
    name = f'pico-page-{version}-{BOARD}-release.bin'
    manifest = dict(manifest=layout.manifest_format, board=BOARD, version=version, published_at=published,
                    url=download_url(base_url, name), size=len(image), sha256=hashlib.sha256(image).hexdigest(),
                    data_schema=dict(progress=schema[0], config=schema[1]), notes=notes)
    if layout == OTA7:
        manifest['layout'] = layout.name
    if notes_i18n is not None:
        if layout != OTA7 or not isinstance(notes_i18n, dict) or set(notes_i18n) != set(TRANSLATED_LANGUAGES):
            raise ValueError('Localized OTA7 manifests require zh-Hant and en release notes')
        for language in TRANSLATED_LANGUAGES:
            validate_notes(notes_i18n[language], language, required=True)
        manifest['notes_i18n'] = notes_i18n
    serialize_manifest(manifest)
    return name, manifest


def full_name(version):
    return f'pico-page-{version}-{BOARD}-release-full.bin'


def layout_partitions(layout):
    other_slot = APP_OFFSET + layout.slot_bytes
    return (
        ('nvs', 'data', 'nvs', 0x9000, 0x6000, ''),
        ('phy_init', 'data', 'phy', 0xF000, 0x1000, ''),
        ('ota_0', 'app', 'ota_0', APP_OFFSET, layout.slot_bytes, ''),
        ('ota_1', 'app', 'ota_1', other_slot, layout.slot_bytes, ''),
        ('otadata', 'data', 'ota', other_slot + layout.slot_bytes, 0x2000, ''),
    )


def verify_table(table, layout):
    expected_rows = layout_partitions(layout)
    if layout == OTA7:
        with (ROOT / 'firmware/partitions.build.csv').open(encoding='utf-8', newline='') as partition_csv:
            rows = tuple((name, kind, subtype, int(start, 0), int(size, 0), flags)
                         for name, kind, subtype, start, size, flags in csv.reader(
                             line for line in partition_csv if line.strip() and not line.lstrip().startswith('#')))
        if rows != expected_rows:
            raise ValueError('Partition CSV differs from the published ota7 layout')

    types = {'app': 0, 'data': 1}
    subtypes = {'ota_0': 0x10, 'ota_1': 0x11, 'ota': 0, 'phy': 1, 'nvs': 2}
    expected = [(name.encode().ljust(16, b'\0'), types[kind], subtypes[subtype], start, size, 0)
                for name, kind, subtype, start, size, _ in expected_rows]
    if len(table) > TABLE_BYTES or len(table) % 32:
        raise ValueError('Invalid partition table size')
    entries = []
    for offset in range(0, len(table), 32):
        magic, kind, subtype, start, size, label, flags = struct.unpack_from('<HBBII16sI', table, offset)
        if magic == 0xEBEB:
            if (table[offset + 2:offset + 16] != b'\xff' * 14
                    or table[offset + 16:offset + 32] != hashlib.md5(table[:offset]).digest()):
                raise ValueError('Partition table digest does not match its entries')
            if table[offset + 32:].strip(b'\xff'):
                raise ValueError('Partition table has data after its digest')
            break
        if magic != 0x50AA:
            raise ValueError('Partition table has no digest')
        entries.append((label, kind, subtype, start, size, flags))
    else:
        raise ValueError('Partition table has no digest')
    previous_end = TABLE_OFFSET + TABLE_BYTES
    for _, kind, _, start, size, _ in entries:
        alignment = 0x10000 if kind == 0 else 0x1000
        if start % alignment or size % 0x1000 or not size or start < previous_end or start + size > FLASH_BYTES:
            raise ValueError('Partition table has an unaligned, overlapping, or out-of-bounds entry')
        previous_end = start + size
    if entries != expected:
        raise ValueError(f'Partition table does not match the {layout.name} application layout')


def build_full(bootloader, table, application):
    if len(bootloader) > TABLE_OFFSET or len(table) > TABLE_BYTES:
        raise ValueError('Bootloader or partition table exceeds its reserved space')
    full = (bootloader.ljust(TABLE_OFFSET, b'\xff') + table.ljust(TABLE_BYTES, b'\xff')).ljust(APP_OFFSET, b'\xff') + application
    verify_full(full, application)
    return full


def verify_full(full, application):
    if len(full) <= APP_OFFSET:
        raise ValueError('Full image is truncated')
    end = verify_image(full[:TABLE_OFFSET])
    # The installer keeps the flash settings of the bootloader header; byte 3 holds the flash size (4 = 16 MiB).
    if struct.unpack_from('<H', full, 12)[0] != ESP32S3_CHIP_ID or full[3] >> 4 != 4:
        raise ValueError('Bootloader is not built for ESP32-S3 with 16 MiB flash')
    _, layout = image_details(application)
    if len(application) > layout.slot_bytes:
        raise ValueError(f'Application does not fit the {layout.name} layout')
    verify_table(full[TABLE_OFFSET:TABLE_OFFSET + TABLE_BYTES], layout)
    if full[end:TABLE_OFFSET].strip(b'\xff') or full[TABLE_OFFSET + TABLE_BYTES:APP_OFFSET].strip(b'\xff'):
        raise ValueError('Full image has data outside the bootloader, partition table and application')
    if full[APP_OFFSET:] != application:
        raise ValueError('Full image does not hold the application the manifest describes')


def newest_release(releases, after_version=None):
    def order(tag):
        core, _, stage = tag[1:].partition('-')
        name, _, number = stage.partition('.')
        return [int(part) for part in core.split('.')] + [('beta', 'rc', '').index(name), int(number or 0)]
    tags = [entry['tagName'] for entry in releases
            if not entry.get('isDraft') and entry['tagName'][:1] == 'v' and RELEASE.fullmatch(entry['tagName'][1:])]
    if after_version is not None:
        if not RELEASE.fullmatch(after_version):
            raise ValueError('Expected a release version without the v prefix')
        minimum = order('v' + after_version)
        tags = [tag for tag in tags if order(tag) > minimum]
    return max(tags, key=order, default=None)


def check_directory(directory, base_url, *, expected_layout=None, require_full=False):
    """Verify that a published directory holds one manifest that describes its application image.

    Optionally require a layout name and the matching full-flash image. Returns the published
    version, or None when the directory publishes nothing and no full image was required.
    """
    manifest = checked_manifest(directory, base_url, expected_layout, require_full)
    return manifest['version'] if manifest is not None else None


def checked_manifest(directory, base_url, expected_layout, require_full):
    images = sorted(path.name for path in directory.glob('*.bin')) if directory.is_dir() else []
    path = directory / 'manifest.json'
    if not path.is_file():
        if images:
            raise ValueError('Images without manifest.json: ' + ', '.join(images))
        if require_full:
            raise ValueError('Firmware manifest is required')
        return None
    payload = path.read_bytes()
    if len(payload) > MANIFEST_LIMIT:
        raise ValueError('Manifest exceeds the 32 KiB HTTP response limit')
    manifest = json.loads(payload.decode('utf-8'))
    schema = manifest.get('data_schema') if isinstance(manifest, dict) else None
    if (not isinstance(schema, dict) or type(manifest.get('manifest')) is not int
            or not all(type(schema.get(key)) is int and schema[key] >= 1 for key in ('progress', 'config'))
            or not all(isinstance(manifest.get(key), str) for key in ('url', 'notes', 'published_at'))):
        raise ValueError('manifest.json lacks url, notes, published_at or data_schema')
    name = manifest['url'].rsplit('/', 1)[-1]
    if not (directory / name).is_file():
        raise ValueError('manifest.json names ' + name + ', which is not in ' + str(directory))
    application = (directory / name).read_bytes()
    expected = build_manifest(application, manifest['notes'], base_url, manifest['published_at'],
                              (schema['progress'], schema['config']), notes_i18n=manifest.get('notes_i18n'))[1]
    layout = expected.get('layout', LEGACY.name)
    if expected_layout is not None and layout != expected_layout:
        raise ValueError(f'Expected {expected_layout} firmware layout, found {layout}')
    if manifest != expected:
        differing = sorted(key for key in set(manifest) | set(expected) if manifest.get(key) != expected.get(key))
        raise ValueError('manifest.json does not describe ' + name + ': ' + ', '.join(differing))
    full = full_name(manifest['version'])
    if set(images) - {name, full}:
        raise ValueError('Images that the manifest does not name: ' + ', '.join(sorted(set(images) - {name, full})))
    if full in images:
        verify_full((directory / full).read_bytes(), application)
    elif require_full:
        raise ValueError('Missing full-flash image ' + full)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', type=Path, metavar='DIR', help='verify a published directory instead of writing one')
    parser.add_argument('--expect-version', help='with --check: the version that must be published')
    parser.add_argument('--expect-layout', choices=[LEGACY.name, OTA7.name],
                        help='with --check: require this layout (default: legacy)')
    parser.add_argument('--require-full', action='store_true', help='with --check: require the full-flash image')
    parser.add_argument('--pick-release', type=Path, metavar='JSON',
                        help='print the newest release tag from a GitHub release list with tagName and optional isDraft')
    parser.add_argument('--after-version', help='with --pick-release: only tags above this release version')
    parser.add_argument('--image', type=Path, help='Pico Page release application image')
    parser.add_argument('--full', action='store_true',
                        help='also write the full-flash image from the bootloader and partition table of the same build')
    parser.add_argument('--notes', type=Path, help='Markdown release notes, UTF-8')
    parser.add_argument('--notes-hant', type=Path, help='traditional Chinese release notes, UTF-8')
    parser.add_argument('--notes-en', type=Path, help='English release notes, UTF-8')
    parser.add_argument('--base-url', default='https://picopage.wulu.zone/firmware/')
    parser.add_argument('--publish-base-url', help='with --check: publish the verified manifest using this download origin')
    parser.add_argument('--published', default=datetime.date.today().isoformat(), help='YYYY-MM-DD')
    parser.add_argument('--out', type=Path, help='directory for manifest.json and the named image')
    arguments = parser.parse_args()
    if arguments.publish_base_url and not arguments.check:
        parser.error('--publish-base-url requires --check')
    if (arguments.expect_layout or arguments.require_full) and not arguments.check:
        parser.error('--expect-layout and --require-full require --check')
    if arguments.after_version and not arguments.pick_release:
        parser.error('--after-version requires --pick-release')
    if bool(arguments.notes_hant) != bool(arguments.notes_en) or (
            arguments.notes_hant and (not arguments.notes or arguments.check or arguments.pick_release)):
        parser.error('--notes-hant and --notes-en must be supplied together with --notes for generation')
    if arguments.pick_release:
        try:
            print(newest_release(json.loads(arguments.pick_release.read_text(encoding='utf-8')),
                                 arguments.after_version) or '')
        except (OSError, ValueError) as error:
            sys.exit(str(error))
        return
    if arguments.check:
        try:
            manifest = checked_manifest(arguments.check, arguments.base_url,
                                        arguments.expect_layout or LEGACY.name, arguments.require_full)
        except (OSError, ValueError) as error:
            sys.exit(str(error))
        version = manifest['version'] if manifest is not None else None
        if arguments.expect_layout and not version:
            sys.exit('Firmware manifest is required')
        if arguments.expect_version and version != arguments.expect_version:
            sys.exit('Expected firmware ' + arguments.expect_version + ', found ' + (version or 'none'))
        if version and arguments.publish_base_url:
            try:
                path = arguments.check / 'manifest.json'
                filename = manifest['url'].rsplit('/', 1)[-1]
                manifest['url'] = download_url(arguments.publish_base_url, filename)
                temporary = path.with_suffix('.json.tmp')
                temporary.write_text(serialize_manifest(manifest), encoding='utf-8')
                temporary.replace(path)
            except (OSError, ValueError) as error:
                sys.exit(str(error))
        print('Firmware ' + version + ' matches its manifest' if version else 'No firmware published')
        return
    if not (arguments.image and arguments.notes and arguments.out):
        parser.error('--image, --notes and --out are required unless --check is given')
    try:
        image = arguments.image.read_bytes()
        translations = None
        if arguments.notes_hant:
            translations = {language: path.read_text(encoding='utf-8') for language, path in
                            zip(TRANSLATED_LANGUAGES, (arguments.notes_hant, arguments.notes_en))}
        name, manifest = build_manifest(image, arguments.notes.read_text(encoding='utf-8'),
                                        arguments.base_url, arguments.published, schema_defaults(), notes_i18n=translations)
        build = arguments.image.parent
        full = arguments.full and build_full((build / 'bootloader/bootloader.bin').read_bytes(),
                                             (build / 'partition_table/partition-table.bin').read_bytes(), image)
    except (OSError, ValueError) as error:
        sys.exit(str(error))
    arguments.out.mkdir(parents=True, exist_ok=True)
    for stale in arguments.out.glob('pico-page-*-' + BOARD + '-release*.bin'):
        stale.unlink()
    (arguments.out / name).write_bytes(image)
    if full:
        (arguments.out / full_name(manifest['version'])).write_bytes(full)
    (arguments.out / 'manifest.json').write_text(serialize_manifest(manifest), encoding='utf-8')
    print(arguments.out / 'manifest.json')


if __name__ == '__main__':
    main()
