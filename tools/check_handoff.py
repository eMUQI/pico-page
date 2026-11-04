#!/usr/bin/env python3
from __future__ import annotations
import json
import os
import re
import struct
import sys
from pathlib import Path
from urllib.parse import unquote, urlsplit

if sys.version_info < (3, 11):
    print('ERROR: Python 3.11+ is required for standard-library tomllib.', file=sys.stderr)
    raise SystemExit(2)
import tomllib

ROOT = Path(__file__).resolve().parents[1]


def project_files():
    excluded = {'build', 'dist', 'third_party', 'managed_components', 'node_modules', 'local', 'private', 'artifacts', '__pycache__'}
    for directory, dirs, files in os.walk(ROOT):
        dirs[:] = [name for name in dirs if not name.startswith('.') and name not in excluded]
        for name in files:
            yield Path(directory) / name


def local_path(relative: str) -> Path:
    rel = Path(relative)
    if rel.is_absolute() or '..' in rel.parts:
        raise ValueError(f'Unsafe resource path: {relative}')
    p = (ROOT / rel).resolve()
    if p != ROOT and ROOT not in p.parents:
        raise ValueError(f'Path escapes repository: {relative}')
    return p


def main() -> int:
    errors: list[str] = []
    checks = 0

    def check(ok: bool, message: str) -> None:
        nonlocal checks
        checks += 1
        if not ok:
            errors.append(message)

    inv = json.loads((ROOT / 'docs/design/inventory.json').read_text(encoding='utf-8'))
    check(len(inv['pngs']) == 36, 'Original PNG count must be 36')
    check(inv['declared_states_count'] == 69, 'Original declared state count must be 69')
    check(len(inv['states_without_png']) == 33, 'Original PNG coverage gap must remain explicit')
    for entry in inv['pngs']:
        p = local_path(entry['path'])
        if not p.is_file():
            check(False, f'Missing PNG: {entry["path"]}')
            continue
        b = p.read_bytes()
        valid = len(b) >= 24 and b[:8] == b'\x89PNG\r\n\x1a\n'
        check(valid, f'Invalid PNG: {entry["path"]}')
        if valid:
            check(struct.unpack('>II', b[16:24]) == (684, 1216), f'Unexpected dimensions: {entry["path"]}')

    examples: dict[str, dict] = {}
    for rel in ['examples/sdcard/PicoPage/settings.toml',
                'examples/sdcard/PicoPage/integrations.toml',
                'examples/sidecar/settings.toml']:
        try:
            examples[rel] = tomllib.loads(local_path(rel).read_text(encoding='utf-8'))
            check(examples[rel].get('schema_version') == 1, f'Unexpected TOML schema: {rel}')
        except (OSError, ValueError) as exc:
            check(False, f'Invalid TOML {rel}: {exc}')
    integration = examples.get('examples/sdcard/PicoPage/integrations.toml', {}).get('kosync', {})
    check(integration.get('enabled') is False, 'Example sync must be disabled')
    check(integration.get('server_url') == '', 'Example must not target a live server')
    check(integration.get('credential_ref') == '', 'Example must not contain a configured credential reference')

    for p in project_files():
        if p.suffix != '.md':
            continue
        rel = p.relative_to(ROOT).as_posix()
        if rel.startswith('docs/design/original/'):
            continue
        text = re.sub(r'```.*?```', '', p.read_text(encoding='utf-8'), flags=re.S)
        for target in re.findall(r'!?\[[^\]]*\]\(([^)]+)\)', text):
            target = target.strip()
            if urlsplit(target).scheme or target.startswith('#'):
                continue
            target = unquote(target.split('#', 1)[0])
            check((p.parent / target).exists(), f'Broken local link in {rel}: {target}')

    agents = (ROOT / 'AGENTS.md').read_text(encoding='utf-8')
    check('所有可交互页面 K2 = ForceRefresh' in agents, 'Missing global K2 full-refresh rule')
    check('NVS 不保存隐藏的权威阅读配置/进度' in agents, 'Missing file-first storage rule')
    check(len(agents.encode('utf-8')) < 16000, 'Root AGENTS exceeds the size limit')
    font_ext = {'.ttf', '.otf', '.ttc', '.otc', '.woff', '.woff2', '.dfont'}
    licensed_fallback = ROOT / 'firmware/components/reader_graphics/fonts/PicoPageFallback.otf'
    welcome_subset = licensed_fallback.parent / "PicoPageWelcome.ttf"
    check((licensed_fallback.parent / "LXGW-OFL.txt").is_file(), "Missing welcome font license")
    web_sample = ROOT / 'web/public/assets/fonts/PicoPageWenKaiSample.woff'
    check(all(p in (licensed_fallback, welcome_subset, web_sample) for p in project_files() if p.suffix.lower() in font_ext),
          'Unexpected font file outside the reviewed embedded UI/welcome subsets and the website sample')
    check((ROOT / 'web/public/downloads/LXGWWenKai-v1.520-OFL.txt').is_file(), 'Missing website sample font license')
    check((licensed_fallback.parent / 'Noto-OFL.txt').is_file(), 'Missing embedded font license')

    if errors:
        print(f'FAIL: {len(errors)} failures across {checks} checks.', file=sys.stderr)
        for message in errors:
            print(' - ' + message, file=sys.stderr)
        return 1
    print(f'PASS: {checks} documentation checks.')
    print('36 original design PNGs are 684x1216; declared coverage gaps remain explicit.')
    print('3 TOML examples parsed; current local documentation links resolved.')
    print('This does NOT validate firmware builds, hardware, EPUB rendering, or KOReader synchronization.')
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(f'ERROR: Documentation validation could not complete: {exc}', file=sys.stderr)
        raise SystemExit(1)
