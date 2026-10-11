#!/usr/bin/env python3
"""Resolve the product version and identify unpublished builds."""
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
NUMBER = r'(?:0|[1-9][0-9]*)'
VERSION = re.compile(rf'{NUMBER}\.{NUMBER}\.{NUMBER}(?:-(?:beta|rc)\.[1-9][0-9]*)?')


def project_version(root=ROOT, require_release=False):
    base = (root / 'firmware/version.txt').read_text().strip()
    if not VERSION.fullmatch(base):
        raise ValueError('version.txt must contain X.Y.Z, X.Y.Z-beta.N or X.Y.Z-rc.N')

    def git(*args):
        return subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()

    commit = git('rev-parse', 'HEAD')
    dirty = bool(git('status', '--porcelain', '--untracked-files=normal'))
    tags = git('tag', '--points-at', 'HEAD').splitlines()
    released = not dirty and 'v' + base in tags
    if require_release and (not released or any(tag != 'v' + base for tag in tags if tag.startswith('v'))):
        raise ValueError('Release requires a clean checkout and a matching v<version> tag at HEAD')
    result = base if released else base + '+g' + commit[:7] + ('.dirty' if dirty else '')
    # ESP-IDF stores a NUL-terminated version in a 32-byte application descriptor field.
    if len(result) > 31:
        raise ValueError('Application version exceeds the 31-byte ESP-IDF limit')
    return result


if __name__ == '__main__':
    if sys.argv[1:] not in ([], ['--check-release']):
        sys.exit('Usage: project_version.py [--check-release]')
    print(project_version(require_release=bool(sys.argv[1:])))
