#!/usr/bin/env python3
import argparse
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "firmware/components/reader_core"
PLATFORM = ROOT / "firmware/components/platform_read_pico"
LOCALES = (("zh-Hant", "translations.inc", "hant"), ("en", "translations_en.inc", "en"))


def unique_messages(pairs):
    messages = {}
    for key, value in pairs:
        if key in messages:
            raise ValueError(f"Duplicate message key: {key}")
        if not isinstance(value, str) or not key or not value or "\0" in key + value:
            raise ValueError(f"Invalid translation: {key!r}")
        messages[key] = value
    return messages


def read_catalog(path):
    return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique_messages)


def validate_placeholders(messages, label):
    for source, translated in messages.items():
        if sorted(re.findall(r"\{[a-zA-Z_]\w*\}", source)) != sorted(re.findall(r"\{[a-zA-Z_]\w*\}", translated)):
            raise SystemExit(f"Placeholder mismatch in {label}: {source}")


def write_generated(path, contents, check):
    if check:
        if not path.exists() or path.read_text(encoding="utf-8") != contents:
            raise SystemExit(f"{path.name} is stale; run python3 tools/generate_translations.py")
    else:
        path.write_text(contents, encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description="Generate compiled interface translations and transfer pages.")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    catalogs = {language: read_catalog(COMPONENT / f"locales/{language}.json") for language, _, _ in LOCALES}
    keys = set(catalogs["zh-Hant"])
    for language, messages in catalogs.items():
        if set(messages) != keys:
            raise SystemExit(f"Message keys differ for {language}: {sorted(keys ^ set(messages))}")
        validate_placeholders(messages, language)
    message_paths = [path for name in ("reader_app", "reader_sync")
                     for path in (ROOT / "firmware/components" / name).glob("*.*")]
    for path in message_paths:
        if path.suffix not in {".cpp", ".h"}:
            continue
        for literal in re.findall(r'\btrf?\(("(?:[^"\\]|\\.)*")\s*[,)]', path.read_text(encoding="utf-8")):
            key = json.loads(literal)
            if key not in keys:
                raise SystemExit(f"Missing translation in {path.name}: {key}")
    for name in ("reader_document/document.cpp", "reader_app/content_cache.cpp"):
        path = ROOT / "firmware/components" / name
        for literal in re.findall(r'"(?:[^"\\]|\\.)*"', path.read_text(encoding="utf-8")):
            if "TXT" not in literal or not re.search(r'[\u3400-\u9fff]', literal):
                continue
            key = json.loads(literal)
            if key not in keys:
                raise SystemExit(f"Missing TXT diagnostic translation in {path.name}: {key}")
    template = (PLATFORM / "upload_page.h").read_text(encoding="utf-8").split('R"HTML(', 1)[1].split(')HTML"', 1)[0]
    phrases = set(re.findall(r'(?:\{[a-zA-Z_]\w*\})*[\u3400-\u9fff][^<>"\x27`\n]*', template))
    for language, table_name, page_suffix in LOCALES:
        contents = "".join("    {" + json.dumps(key, ensure_ascii=False) + ", " +
                           json.dumps(value, ensure_ascii=False) + "},\n"
                           for key, value in sorted(catalogs[language].items()))
        upload = read_catalog(PLATFORM / f"upload-{language}.json")
        if phrases != set(upload):
            raise SystemExit(f"Upload page translation keys differ for {language}: {sorted(phrases ^ set(upload))}")
        validate_placeholders(upload, f"upload-{language}")
        source = template
        for key, value in sorted(upload.items(), key=lambda item: len(item[0]), reverse=True):
            source = source.replace(key, value)
        source = source.replace('lang="zh-CN"', f'lang="{language}"')
        page = '#pragma once\n\nnamespace pico_page::platform {\ninline constexpr char upload_page_' + page_suffix + '[] = R"HTML(' + source + ')HTML";\n}\n'
        write_generated(COMPONENT / table_name, contents, args.check)
        write_generated(PLATFORM / f"upload_page_{page_suffix}.h", page, args.check)


if __name__ == "__main__":
    main()
