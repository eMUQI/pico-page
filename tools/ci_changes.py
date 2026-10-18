#!/usr/bin/env python3
"""Select PR checks or compare website inputs; manual runs select all checks."""
import argparse
import os
import subprocess


WEB_FILES = {'.github/workflows/web.yml', 'tools/ota_manifest.py', 'tools/ci_changes.py'}


def checks_for_paths(paths):
    # Unclassified paths require host checks so new source directories are covered by default.
    host = any(not (path.startswith(('docs/', 'artifacts/', 'web/'))
                    or path.endswith('.md') or path == '.github/workflows/web.yml')
               for path in paths)
    web = any(path.startswith(('web/', 'docs/releases/')) or path in WEB_FILES for path in paths)
    return {'host': host, 'web': web}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    comparison = parser.add_mutually_exclusive_group()
    comparison.add_argument('--base', default='')
    comparison.add_argument('--compare', default='', help='Compare HEAD directly with a deployment successor.')
    args = parser.parse_args()
    checks = {'host': True, 'web': True}
    if args.base or args.compare:
        # Deployment freshness compares trees; PR selection uses the merge base.
        revisions = [f'{args.base}...HEAD'] if args.base else ['HEAD', args.compare]
        # NUL separators and disabled rename detection preserve unusual names and both rename paths.
        changed = subprocess.check_output([
            'git', 'diff', '--name-only', '--no-renames', '-z', *revisions, '--',
        ])
        checks = checks_for_paths([os.fsdecode(path) for path in changed.split(b'\0') if path])
    for name, selected in checks.items():
        print(f'{name}={str(selected).lower()}')


if __name__ == '__main__':
    main()
