#!/usr/bin/env python3
"""Bind successful firmware builds to source contents and artifact hashes."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

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


def artifacts(build):
    config = build / 'sdkconfig'
    if not config.is_file() and build.resolve() == ROOT / 'firmware/build':
        config = ROOT / 'firmware/sdkconfig'
    paths = {'pico_page.bin': build / 'pico_page.bin', 'pico_page.elf': build / 'pico_page.elf', 'sdkconfig': config}
    return {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in paths.items() if path.is_file()}, config


def unapplied_defaults(config, defaults):
    """Returns the options whose value in the generated sdkconfig differs from the defaults files; later files win."""
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


def require_defaults(build):
    """An existing sdkconfig takes precedence over defaults, so a stale one silently drops later defaults changes."""
    _, config = artifacts(build)
    if not config.is_file():
        return
    defaults = [ROOT / 'firmware/sdkconfig.defaults']
    mode = build.resolve().name.removeprefix('firmware-')
    if build.resolve().parent == ROOT / 'build' and (ROOT / f'firmware/configs/{mode}.defaults').is_file():
        defaults.append(ROOT / f'firmware/configs/{mode}.defaults')
    stale = unapplied_defaults(config, defaults)
    if stale:
        raise RuntimeError(f'{config} does not carry the defaults for {", ".join(stale)}; '
                           'remove it to regenerate from defaults, or update it explicitly')


def main():
    action, directory = sys.argv[1:]
    build = Path(directory)
    build.mkdir(parents=True, exist_ok=True)
    pending, manifest = build / 'source-before.json', build / 'provenance.json'
    if action == 'begin':
        manifest.unlink(missing_ok=True)
        require_defaults(build)
        pending.write_text(json.dumps(source_state()))
    elif action == 'finish':
        before = json.loads(pending.read_text())
        if before != source_state():
            raise RuntimeError('Source changed during firmware build; rebuild before recording evidence')
        hashes, _ = artifacts(build)
        if len(hashes) != 3:
            raise RuntimeError('Firmware build artifacts are incomplete')
        require_defaults(build)
        manifest.write_text(json.dumps(dict(schema=1, **before, artifacts_sha256=hashes), indent=2) + '\n')
    else:
        raise ValueError(action)


if __name__ == '__main__':
    main()
