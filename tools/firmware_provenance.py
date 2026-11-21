#!/usr/bin/env python3
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from ota_manifest import APPLICATION_LAYOUTS, DESCRIPTION, LEGACY
from project_version import project_version

ROOT = Path(__file__).resolve().parents[1]


def source_state():
    def git(*args):
        return subprocess.check_output(['git', '-C', str(ROOT), *args])
    names = set(git('ls-files', '-z', '--cached', '--others', '--exclude-standard').split(b'\0'))
    digest = hashlib.sha256()
    for name in sorted(names - {b''}):
        path = ROOT / name.decode()
        digest.update(name + b'\0')
        digest.update(hashlib.sha256(path.read_bytes()).digest() if path.is_file() else b'missing')
    return dict(source_commit=git('rev-parse', 'HEAD').decode().strip(),
                source_dirty=bool(git('status', '--porcelain').strip()),
                source_sha256=digest.hexdigest())


def application_paths(build):
    metadata_path = build / 'project_description.json'
    if not metadata_path.is_file():
        return build / f'{LEGACY.project}.bin', build / f'{LEGACY.project}.elf'
    metadata = json.loads(metadata_path.read_text(encoding='utf-8'))
    project = metadata.get('project_name') if isinstance(metadata, dict) else None
    if (not isinstance(project, str) or project not in APPLICATION_LAYOUTS
            or metadata.get('app_bin') != f'{project}.bin'
            or metadata.get('app_elf') != f'{project}.elf'):
        raise RuntimeError('Unexpected Pico Page application names in project_description.json')
    return build / metadata['app_bin'], build / metadata['app_elf']


def sdkconfig_path(build):
    config = build / 'sdkconfig'
    if not config.is_file() and build.resolve() == ROOT / 'firmware/build':
        config = ROOT / 'firmware/sdkconfig'
    return config


def artifacts(build):
    config = sdkconfig_path(build)
    paths = (*application_paths(build), config)
    return {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in paths if path.is_file()}, config


def unapplied_defaults(config, defaults):
    wanted = {}
    for path in defaults:
        for line in path.read_text().splitlines():
            line = line.strip()
            if line.startswith('CONFIG_') and '=' in line:
                name, value = line.split('=', 1)
                wanted[name] = None if value == 'n' else value
            elif line.startswith('# CONFIG_') and line.endswith(' is not set'):
                wanted[line[2:-len(' is not set')]] = None
    effective = dict(line.split('=', 1) for line in config.read_text().splitlines()
                     if line.startswith('CONFIG_') and '=' in line)
    return sorted(name for name, value in wanted.items() if effective.get(name) != value)


def require_defaults(build, mode):
    """An existing sdkconfig takes precedence over defaults, so a stale one silently drops later defaults changes."""
    config = sdkconfig_path(build)
    if not config.is_file():
        return
    defaults = [ROOT / 'firmware/board/sdkconfig.defaults']
    profile = ROOT / f'firmware/configs/{mode}.defaults'
    if profile.is_file():
        defaults.append(profile)
    stale = unapplied_defaults(config, defaults)
    if stale:
        raise RuntimeError(f'{config} does not carry the defaults for {", ".join(stale)}; '
                           'remove it to regenerate from defaults, or update it explicitly')


def main():
    action, directory, *explicit_mode = sys.argv[1:]
    if explicit_mode and explicit_mode not in ([''], ['release'], ['performance'], ['diagnostic'], ['stages']):
        sys.exit('Expected one firmware build mode')
    build = Path(directory)
    mode = build.name.removeprefix('firmware-') if build.parent.resolve() == ROOT / 'build' else 'default'
    if explicit_mode:
        mode = explicit_mode[0] or 'default'
    build.mkdir(parents=True, exist_ok=True)
    pending, manifest = build / 'source-before.json', build / 'provenance.json'
    if action == 'begin':
        manifest.unlink(missing_ok=True)
        require_defaults(build, mode)
        pending.write_text(json.dumps(source_state()))
    elif action == 'finish':
        before = json.loads(pending.read_text())
        if before != source_state():
            raise RuntimeError('Source changed during firmware build; rebuild before recording evidence')
        hashes, _ = artifacts(build)
        if len(hashes) != 3:
            raise RuntimeError('Firmware build artifacts are incomplete')
        require_defaults(build, mode)
        version = project_version()
        application, _ = application_paths(build)
        image = application.read_bytes()
        if (image[DESCRIPTION:DESCRIPTION + 4] != bytes.fromhex('3254cdab')
                or image[DESCRIPTION + 16:DESCRIPTION + 48].split(b'\0', 1)[0] != version.encode()
                or image[DESCRIPTION + 48:DESCRIPTION + 80].split(b'\0', 1)[0] != application.stem.encode()):
            raise RuntimeError('Application descriptor version or project differs from the build; reconfigure and rebuild')
        manifest.write_text(json.dumps(dict(schema=1, **before, version=version, build_mode=mode,
                                           artifacts_sha256=hashes), indent=2) + '\n')
    else:
        raise ValueError(action)


if __name__ == '__main__':
    main()
