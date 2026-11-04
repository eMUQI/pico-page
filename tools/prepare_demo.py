#!/usr/bin/env python3
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from make_epub_fixture import fixture_entries, write_fixture


def main():
    card = ROOT / '.local/card'
    (card / 'Books').mkdir(parents=True, exist_ok=True)
    (card / 'Fonts').mkdir(exist_ok=True)
    (card / 'PicoPage').mkdir(exist_ok=True)
    path = card / 'Books/Pico Page 演示.epub'
    if not path.exists():
        paragraphs = [
            '清晨，窗外的树影缓缓移动。桌上的书翻到新的一页，昨天留下的句子仍然清晰。阅读不需要急于走向结尾；停下来的片刻，也属于故事本身。',
            '小路通向河岸。行人从桥上经过，远处传来钟声。这里的每一段文字都是为 Pico Page 编写的演示内容，可以自由用于开发与测试。',
            '翻过这一页，还会有新的风景。中文与 English 123 可以排在同一行；标点、空白和换行共同形成阅读的节奏。',
        ]
        entries = fixture_entries()
        for number, title, count in [(1, '从一页开始', 36), (2, '继续阅读', 24)]:
            entries[f'EPUB/chapter{number}.xhtml'] = (
                '<html xmlns="http://www.w3.org/1999/xhtml"><head><title>' + title + '</title></head>'
                f'<body><h1 id="{"opening" if number == 1 else "second"}">{title}</h1>'
                + ''.join('<p>' + paragraphs[index % 3] + '</p>' for index in range(count))
                + '</body></html>'
            )
        write_fixture(path, entries)
    settings = card / 'PicoPage/settings.toml'
    if not settings.exists():
        settings.write_text(
            'schema_version = 1\n[reader]\nfont = "Fonts/LXGWWenKai-Regular.ttf"\n'
            'font_size_px = 38\nline_height = 1.5\nmargin_x_px = 36\n', encoding='utf-8'
        )
    print(f'Demonstration card ready: {card}')


if __name__ == '__main__':
    main()
