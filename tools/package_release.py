#!/usr/bin/env python3
"""Build Pico Page from a release tag and prepare its four GitHub Release assets."""

import argparse
import datetime
import hashlib
from io import BytesIO
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import tempfile

from firmware_provenance import application_paths, artifacts, source_state
import ota_manifest
from release_notes import TRANSLATED_LANGUAGES, read_release_notes
from project_version import ROOT, project_version
from setup_fonts import ASSETS, REVISION as FREETYPE_REVISION


BOARD = ROOT / 'firmware/board'
IDF_PATCH = BOARD / 'patches/esp-idf-ext-ram-linker.patch'
BSP = ROOT / '.local/read_pico_firmware'
FREETYPE = ROOT / '.local' / f'freetype-{FREETYPE_REVISION}'
BASE_URL = 'https://picopage.wulu.zone/firmware/'
PROJECT_URL = 'https://github.com/eMUQI/pico-page'
IDF_URL = 'https://github.com/espressif/esp-idf'
BSP_URL = 'https://github.com/MindReset/read_pico_firmware'


def git(directory, *args):
    return subprocess.check_output(['git', '-C', str(directory), *args])


def verify_sdk(sdk, revision):
    if git(sdk, 'rev-parse', 'HEAD').decode().strip() != revision:
        raise ValueError(f'ESP-IDF must be checked out at {revision}')
    patch = git(sdk, 'diff', '--no-color', '--no-ext-diff', '--no-textconv', '--no-renames',
                '--ignore-submodules=none', '--src-prefix=a/', '--dst-prefix=b/', '--unified=1', 'HEAD', '--')

    def patch_lines(data):
        return [line for line in data.splitlines() if not line.startswith(b'index ')]

    if patch_lines(patch) != patch_lines(IDF_PATCH.read_bytes()):
        raise ValueError(f'ESP-IDF tracked changes differ from {IDF_PATCH.relative_to(ROOT)}')
    untracked = set(git(sdk, 'ls-files', '--others', '--exclude-standard', '-z').split(b'\0'))
    if untracked - {b'', b'tools/requirements/requirements.merged.txt'}:
        raise ValueError('ESP-IDF contains unexpected untracked files')
    submodules = git(sdk, 'submodule', 'status', '--recursive').decode().splitlines()
    if any(not entry.startswith(' ') for entry in submodules):
        raise ValueError('ESP-IDF submodules must match their pinned commits')
    return [' '.join(entry.split()[:2]) for entry in submodules]


def sha256(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def verify_freetype():
    name, url, digest = next(asset for asset in ASSETS if asset[0] == 'freetype.tar.gz')
    archive = ROOT / '.local/downloads' / name
    if not archive.is_file() or sha256(archive) != digest or not FREETYPE.is_dir():
        raise ValueError('Prepare the pinned FreeType inputs with python3 tools/setup_fonts.py')
    expected = set()
    with tarfile.open(archive, 'r:gz') as source:
        for member in source:
            if not member.isfile():
                continue
            relative = Path(*Path(member.name).parts[1:])
            path = FREETYPE / relative
            expected.add(relative)
            if not path.resolve().is_relative_to(FREETYPE.resolve()) or path.is_symlink():
                raise ValueError(f'Unexpected FreeType source path: {relative}')
            with source.extractfile(member) as original:
                if sha256(path) != hashlib.file_digest(original, 'sha256').hexdigest():
                    raise ValueError(f'FreeType source differs from the pinned archive: {relative}')
    actual = {path.relative_to(FREETYPE) for path in FREETYPE.rglob('*') if path.is_file() or path.is_symlink()}
    if actual != expected:
        raise ValueError('FreeType source tree contains files outside the pinned archive')
    return url, digest


def read_provenance(build, version, source):
    path = build / 'provenance.json'
    provenance = json.loads(path.read_text(encoding='utf-8'))
    hashes, _ = artifacts(build)
    if (provenance.get('schema') != 1 or provenance.get('version') != version
            or provenance.get('build_mode') != 'release' or provenance.get('artifacts_sha256') != hashes
            or any(provenance.get(key) != value for key, value in source.items())):
        raise ValueError('Release build provenance does not match the tagged source and output')
    return path


def tracked_paths(directory, *pathspec):
    files = git(directory, 'ls-files', '--recurse-submodules', '-z', '--', *pathspec).split(b'\0')
    return [Path(os.fsdecode(name)) for name in files if name]


def add_release_file(archive, root, relative, destination):
    path = root / relative
    if not path.resolve().is_relative_to(root.resolve()) or not path.is_file():
        raise ValueError(f'Invalid release materials file: {relative}')
    add_release_bytes(archive, destination, path.read_bytes())


def add_release_bytes(archive, destination, contents):
    member = tarfile.TarInfo(destination)
    member.mode = 0o644
    member.size = len(contents)
    archive.addfile(member, BytesIO(contents))


def is_license_file(path):
    name = path.name.casefold()
    words = ('license', 'licence', 'copying', 'copyright', 'notice', 'ofl', 'ftl')
    return (any(name.startswith(word) or f'-{word}' in name for word in words)
            or name.startswith('apache-2.0') or name.endswith('-mit.txt'))


def add_licenses(archive, sdk, compiler, managed_license_paths):
    managed = ROOT / 'firmware/managed_components'
    toolchain = compiler.parents[1] / 'share/licenses'
    roots = (
        ('project', ROOT, tracked_paths(ROOT, 'firmware/components')),
        ('esp-idf', sdk, tracked_paths(sdk)),
        ('read-pico-bsp', BSP, tracked_paths(BSP, 'components')),
        ('managed-components', managed, managed_license_paths),
        ('freetype', FREETYPE, [path.relative_to(FREETYPE) for path in FREETYPE.rglob('*')]),
        ('xtensa-toolchain', toolchain, [path.relative_to(toolchain) for path in toolchain.rglob('*')]),
    )
    for name, root, candidates in roots:
        licenses = sorted(relative for relative in candidates if is_license_file(relative) and (root / relative).is_file())
        if not licenses:
            raise ValueError(f'No license files found for {name}')
        for relative in licenses:
            add_release_file(archive, root, relative, f'licenses/{name}/{relative.as_posix()}')
    embedded_notices = (
        'reader_document/third_party/expat',
        'reader_document/third_party/miniz',
        'reader_graphics/third_party/qrcodegen',
        'reader_graphics/third_party/tjpgd',
    )
    components = Path('firmware/components')
    for relative in tracked_paths(ROOT, *(str(components / name) for name in embedded_notices)):
        if relative.suffix in ('.c', '.h'):
            contents = (ROOT / relative).read_bytes()
            notice = re.search(rb'/\*.*?\*/', contents, re.DOTALL)
            if notice and re.search(rb'copyright|license|redistribut', notice[0], re.IGNORECASE):
                add_release_bytes(archive, f'licenses/project/{relative.as_posix()}.txt', notice[0] + b'\n')


def verify_managed_components(lock):
    import yaml
    from idf_component_tools.file_tools import filtered_paths
    from idf_component_tools.hash_tools.errors import ValidatingHashError
    from idf_component_tools.hash_tools.validate import validate_hash_eq_hashdir
    from idf_component_tools.manager import ManifestManager

    managed = ROOT / 'firmware/managed_components'
    licenses = []
    for name, pin in yaml.safe_load(lock)['dependencies'].items():
        if pin['source']['type'] == 'service':
            path = managed / name.replace('/', '__')
            try:
                validate_hash_eq_hashdir(path, pin['component_hash'])
            except ValidatingHashError as error:
                raise ValueError(f'Managed component differs from the lock: {name}') from error
            manifest = ManifestManager(path, name).load()
            files = filtered_paths(path, use_gitignore=manifest.use_gitignore, include=manifest.include_set,
                                   exclude=manifest.exclude_set, exclude_default=False)
            licenses.extend(file.relative_to(managed) for file in files if is_license_file(file))
    return licenses


def write_sbom(description, destination, project_name, private_paths):
    subprocess.run([sys.executable, '-m', 'esp_idf_sbom', 'create', '--format', 'spdx-json@2.2',
                    '--rem-unused', '--file-tags', '--no-sync-excluded-cves',
                    '-o', str(destination), str(description)],
                   cwd=ROOT, check=True)
    contents = destination.read_text(encoding='utf-8')
    if any(str(path) in contents for path in private_paths) or re.search(r'https?://[^/\s"]+@[^/\s"]+/', contents):
        raise ValueError(f'{project_name} SBOM contains a local build path or a URL with credentials')
    sbom = json.loads(contents)
    if sbom.get('name') != project_name or sbom.get('spdxVersion') != 'SPDX-2.2' or not sbom.get('packages'):
        raise ValueError(f'Invalid {project_name} SBOM')


def source_readme(version, commit, sdk_revision, bsp_revision, freetype_url, freetype_digest, compiler_version):
    project = f'{PROJECT_URL}/tree/{commit}'
    return f"""# Pico Page {version} 对应源码与构建资料

- [Pico Page 源码]({project})，发布标签 `v{version}`，提交 `{commit}`。
- [ESP-IDF 源码]({IDF_URL}/tree/{sdk_revision})，提交 `{sdk_revision}`，递归子模块的提交和路径见 `esp-idf-submodules.txt`。编译时应用 `{IDF_PATCH.name}`。
- [Read Pico BSP 源码]({BSP_URL}/tree/{bsp_revision})，提交 `{bsp_revision}`。项目构建入口将标签内的板级补丁应用到 BSP 构建副本。
- [FreeType 源码归档]({freetype_url})，提交 `{FREETYPE_REVISION}`，归档 SHA-256 为 `{freetype_digest}`。
- [ESP-IDF Component Registry](https://components.espressif.com/) 依赖的下载源、固定版本和摘要见 `dependencies.lock`。
- 编译器为 `{compiler_version}`。

## 构建

1. 检出 Pico Page 的 `v{version}` 标签及上述 ESP-IDF 提交，并在 SDK 检出中运行 `git submodule update --init --recursive`。
2. 在独立的 SDK 检出中用 `git apply` 应用本包的 `{IDF_PATCH.name}`，按 ESP-IDF 文档激活工具链。
3. 在 Pico Page 检出中运行 `python3 tools/setup_fonts.py`，再运行 `sh tools/build_firmware.sh release`。请使用全新的构建目录。

构建环境和从源码安装方法见[固件开发指南]({project}/firmware/README.md)。本包中的许可和声明随对应二进制提供。组件审查见[第三方依赖]({project}/docs/development/dependency-review.md)。`SHA256SUMS` 可用于核对同一 Release 的两份固件和 OTA 清单。
"""


def write_materials(destination, build, metadata, sdk, submodules, provenance, release_files, managed_license_paths):
    description = build / 'project_description.json'
    project = json.loads(description.read_text(encoding='utf-8'))
    if Path(project['idf_path']).resolve() != sdk:
        raise ValueError('The Release build used a different ESP-IDF checkout')
    compiler = Path(project['c_compiler']).resolve()
    compiler_version = subprocess.check_output([str(compiler), '--version'], text=True).splitlines()[0]
    with tempfile.TemporaryDirectory(dir=build) as temporary:
        temporary = Path(temporary)
        app_sbom, boot_sbom = temporary / 'application.spdx.json', temporary / 'bootloader.spdx.json'
        private_paths = (ROOT, sdk, compiler.parents[1])
        write_sbom(description, app_sbom, project['project_name'], private_paths)
        write_sbom(build / 'bootloader/project_description.json', boot_sbom, 'bootloader', private_paths)
        with tarfile.open(destination, 'w:gz') as archive:
            add_release_bytes(archive, 'README.md', source_readme(*metadata, compiler_version).encode('utf-8'))
            add_release_bytes(archive, 'esp-idf-submodules.txt', ('\n'.join(submodules) + '\n').encode('utf-8'))
            add_release_bytes(archive, 'SHA256SUMS',
                              ''.join(f'{sha256(path)}  {path.name}\n' for path in sorted(release_files)).encode('utf-8'))
            for path in (ROOT / 'LICENSE', ROOT / 'docs/LICENSING.md', ROOT / 'firmware/dependencies.lock',
                         IDF_PATCH, provenance, app_sbom, boot_sbom):
                add_release_file(archive, path.parent, Path(path.name), path.name)
            add_licenses(archive, sdk, compiler, managed_license_paths)


def package(args):
    version = project_version(require_release=True)
    notes = read_release_notes(version)
    if datetime.date.fromisoformat(args.published).isoformat() != args.published:
        raise ValueError('Use YYYY-MM-DD for --published')
    out = (args.out or ROOT / 'build/release' / version).resolve()
    if out.exists():
        raise ValueError(f'Release output already exists: {out}')
    if out.is_relative_to(ROOT):
        ignored = subprocess.run(['git', 'check-ignore', '-q', '--', str(out)], cwd=ROOT)
        if ignored.returncode != 0:
            raise ValueError('Release output must be outside the checkout or in an ignored directory')
    if not os.environ.get('IDF_PATH'):
        raise ValueError('Activate the pinned ESP-IDF environment before packaging')
    sdk = Path(os.environ['IDF_PATH']).resolve()
    sdk_revision = (BOARD / 'esp-idf-revision.txt').read_text(encoding='utf-8').strip()
    bsp_revision = (BOARD / 'bsp-revision.txt').read_text(encoding='utf-8').strip()
    submodules = verify_sdk(sdk, sdk_revision)
    freetype_url, freetype_digest = verify_freetype()
    subprocess.run([sys.executable, '-m', 'esp_idf_sbom', '--version'], cwd=ROOT, check=True)
    dependency_lock = (ROOT / 'firmware/dependencies.lock').read_text(encoding='utf-8')
    if (ROOT / 'firmware/managed_components').is_dir():
        verify_managed_components(dependency_lock)
    source = source_state()
    (ROOT / 'build').mkdir(exist_ok=True)
    out.parent.mkdir(parents=True, exist_ok=True)
    with (tempfile.TemporaryDirectory(dir=ROOT / 'build', prefix='release-build-') as build_directory,
          tempfile.TemporaryDirectory(dir=out.parent, prefix=f'.{out.name}-') as temporary):
        build = Path(build_directory)
        subprocess.run(['sh', 'tools/build_firmware.sh', 'release', str(build)], cwd=ROOT, check=True)
        if verify_sdk(sdk, sdk_revision) != submodules:
            raise ValueError('ESP-IDF submodules changed during the Release build')
        managed_license_paths = verify_managed_components(dependency_lock)
        provenance = read_provenance(build, version, source)
        app_binary, _ = application_paths(build)
        application = app_binary.read_bytes()
        name, manifest = ota_manifest.build_manifest(
            application, notes['zh-Hans'], BASE_URL, args.published, ota_manifest.schema_defaults(),
            notes_i18n={language: notes[language] for language in TRANSLATED_LANGUAGES})
        if manifest['version'] != version:
            raise ValueError('Application image version differs from the release tag')
        full = ota_manifest.build_full((build / 'bootloader/bootloader.bin').read_bytes(),
                                       (build / 'partition_table/partition-table.bin').read_bytes(), application)
        stage = Path(temporary) / 'assets'
        stage.mkdir()
        app_path, full_path, manifest_path = stage / name, stage / ota_manifest.full_name(version), stage / 'manifest.json'
        app_path.write_bytes(application)
        full_path.write_bytes(full)
        manifest_path.write_text(ota_manifest.serialize_manifest(manifest), encoding='utf-8')
        if ota_manifest.check_directory(stage, BASE_URL, expected_layout=ota_manifest.OTA7.name, require_full=True) != version:
            raise ValueError('Packaged firmware does not match the release tag')
        metadata = (version, source['source_commit'], sdk_revision, bsp_revision, freetype_url, freetype_digest)
        write_materials(stage / f'pico-page-{version}-source-materials.tar.gz', build, metadata,
                        sdk, submodules, provenance, (app_path, full_path, manifest_path), managed_license_paths)
        if project_version(require_release=True) != version or source_state() != source:
            raise ValueError('Tagged source changed while preparing release assets')
        if out.exists():
            raise ValueError(f'Release output already exists: {out}')
        stage.rename(out)
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, help='new output directory (default: build/release/<version>)')
    parser.add_argument('--published', default=datetime.date.today().isoformat(), metavar='YYYY-MM-DD',
                        help='publication date in the OTA manifest (default: today)')
    args = parser.parse_args()
    try:
        print(package(args))
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        sys.exit(f'Release packaging failed: {error}')


if __name__ == '__main__':
    main()
