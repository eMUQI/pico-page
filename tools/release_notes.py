#!/usr/bin/env python3
import argparse
import sys

from project_version import ROOT, VERSION


NOTES_LIMIT = 8 * 1024
TRANSLATED_LANGUAGES = ('zh-Hant', 'en')
LANGUAGE_NAMES = {'zh-Hans': '简体中文', 'zh-Hant': '繁體中文', 'en': 'English'}


def validate_notes(notes, language, *, required=False):
    if not isinstance(notes, str) or '\0' in notes or (required and not notes.strip()):
        raise ValueError(f'Invalid release notes for {language}')
    if len(notes.encode('utf-8')) > NOTES_LIMIT:
        raise ValueError(f'Release notes for {language} exceed 8 KiB')


def read_release_notes(version, directory=ROOT / 'docs/releases'):
    if not VERSION.fullmatch(version):
        raise ValueError('Expected a release version without the v prefix')
    notes = {}
    for language in LANGUAGE_NAMES:
        folder = directory if language == 'zh-Hans' else directory / language
        body = (folder / f'{version}.md').read_text(encoding='utf-8')
        validate_notes(body, language, required=True)
        notes[language] = body
    return notes


def main():
    parser = argparse.ArgumentParser(description='Print the three-language GitHub Release body.')
    parser.add_argument('version', help='release version without the v prefix')
    args = parser.parse_args()
    try:
        notes = read_release_notes(args.version)
        print(' · '.join(f'[{name}](#{name.lower()})' for name in LANGUAGE_NAMES.values()))
        for language, name in LANGUAGE_NAMES.items():
            print(f'\n# {name}\n\n{notes[language].strip()}')
    except (OSError, ValueError) as error:
        sys.exit(str(error))


if __name__ == '__main__':
    main()
