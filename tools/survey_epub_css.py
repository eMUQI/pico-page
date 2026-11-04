#!/usr/bin/env python3
"""Read EPUB CSS usage without modifying archives; keep all output private."""
import argparse
from collections import Counter
import hashlib
from html.parser import HTMLParser
import json
from pathlib import Path
import posixpath
import re
import subprocess
from urllib.parse import unquote, urlsplit
import xml.etree.ElementTree as ET
import zipfile

NUMBER = re.compile(r'[+-]?(?:[0-9]*\.[0-9]+|[0-9]+)(?:[eE][+-]?[0-9]+)?')


class HtmlTree(HTMLParser):
    """Tolerant survey fallback; this does not establish firmware XML acceptance."""
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.root = ET.Element('survey-root')
        self.stack = [self.root]

    def handle_starttag(self, tag, attrs):
        node = ET.SubElement(self.stack[-1], tag, {k: v or '' for k, v in attrs})
        if tag not in ('area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input', 'link', 'meta', 'param', 'source', 'track', 'wbr'):
            self.stack.append(node)

    def handle_endtag(self, tag):
        for i in range(len(self.stack) - 1, 0, -1):
            if self.stack[i].tag == tag:
                del self.stack[i:]
                break

    def handle_data(self, data):
        node = self.stack[-1]
        if node.tag == 'style':
            node.text = (node.text or '') + data

def tokens(source, whitespace=False):
    """Yield lexical tokens; quoted strings and comments cannot form delimiters."""
    i = 0
    while i < len(source):
        c = source[i]
        if source.startswith('/*', i):
            end = source.find('*/', i + 2)
            i = len(source) if end < 0 else end + 2
        elif c.isspace():
            while i < len(source) and source[i].isspace():
                i += 1
            if whitespace:
                yield 'space', ' '
        elif c in '\"\'':
            quote, start = c, i
            i += 1
            while i < len(source):
                if source[i] == '\\':
                    i += 2
                elif source[i] == quote:
                    i += 1
                    break
                else:
                    i += 1
            yield 'string', source[start:i]
        elif match := NUMBER.match(source, i):
            start = i
            i += len(match[0])
            end = i
            while i < len(source) and (source[i].isalpha() or source[i] == '%'):
                i += 1
            yield 'dimension' if end != i else 'number', source[start:i]
        elif c.isalnum() or c in '_-' or (c == '\\' and i + 1 < len(source)) or ord(c) >= 128:
            start = i
            while i < len(source):
                c = source[i]
                if c == '\\' and i + 1 < len(source):
                    i += 2
                    digits = 1 if source[i - 1] in '0123456789abcdefABCDEF' else 6
                    while digits < 6 and i < len(source) and source[i] in '0123456789abcdefABCDEF':
                        i += 1
                        digits += 1
                    if digits < 6 and i < len(source) and source[i].isspace():
                        i += 1
                elif c.isalnum() or c in '_-' or ord(c) >= 128:
                    i += 1
                else:
                    break
            yield 'word', source[start:i]
        else:
            yield c, c
            i += 1


def selector_usage(prelude):
    counts = Counter()
    nesting, simple_count, previous = 0, 0, None
    labels = {'.': 'class', '#': 'id', ':': 'pseudo', '[': 'attribute', '*': 'universal'}
    for index, (kind, _) in enumerate(prelude):
        if kind in (']', ')'):
            nesting = max(0, nesting - 1)
            continue
        if nesting:
            if kind in ('[', '('):
                nesting += 1
            continue
        if kind == 'space':
            if index and index + 1 < len(prelude):
                before, after = prelude[index - 1][0], prelude[index + 1][0]
                if before not in (',', '>', '+', '~', 'space') and after not in (',', '>', '+', '~', 'space'):
                    counts['descendant'] += 1
            simple_count, previous = 0, None
            continue
        if kind in (',', '>', '+', '~'):
            counts[{',': 'group', '>': 'child', '+': 'adjacent', '~': 'sibling'}[kind]] += 1
            simple_count, previous = 0, kind
            continue
        simple = kind in labels or (kind == 'word' and previous not in ('.', '#', ':', '|'))
        if kind in labels:
            # A pseudo-element's second colon is part of the same selector component.
            if kind != ':' or previous != ':':
                counts[labels[kind]] += 1
            else:
                simple = False
        elif simple:
            counts['type'] += 1
        if simple:
            simple_count += 1
            if simple_count == 2:
                counts['compound'] += 1
        if kind in ('[', '('):
            nesting += 1
        previous = kind
    return counts


def css_usage(source):
    ts = list(tokens(source))
    properties, at_rules, units, selectors, shorthands = [Counter() for _ in range(5)]
    stack, depth, important = [], 0, 0
    for i, (kind, value) in enumerate(ts):
        if kind in ('{', '(', '['):
            stack.append(kind)
            depth = max(depth, len(stack))
        elif kind in ('}', ')', ']'):
            if stack:
                stack.pop()
        elif kind == '@' and i + 1 < len(ts):
            at_rules[ts[i + 1][1].lower()] += 1
        elif kind == '!' and i + 1 < len(ts) and ts[i + 1][1].lower() == 'important':
            important += 1
        elif kind == ':' and i and ts[i - 1][0] == 'word' and stack and stack[-1] == '{':
            prop = ts[i - 1][1].lower()
            properties[prop] += 1
            if prop in ('margin', 'padding', 'font', 'border', 'background'):
                end = i + 1
                while end < len(ts) and ts[end][0] not in (';', '}'):
                    end += 1
                # Count shapes only; never include a private font name or URL.
                shape = ' '.join(k for k, _ in ts[i + 1:end])
                shorthands[prop + ':' + shape] += 1
        if kind == 'dimension':
            for unit in ('rem', 'px', 'em', 'pt', 'cm', 'mm', 'in', 'vh', 'vw'):
                if value.lower().endswith(unit):
                    units[unit] += 1
                    break
            if value.endswith('%'):
                units['%'] += 1
    # Group at-rules contain rules; other blocks contain declarations.
    contexts, prelude, rules = [], [], 0
    for kind, value in tokens(source, whitespace=True):
        if kind == '{':
            significant = [(k, v) for k, v in prelude if k != 'space']
            at_rule = bool(significant and significant[0][0] == '@')
            in_declarations = bool(contexts and contexts[-1] == 'declarations')
            if not at_rule and not in_declarations:
                rules += 1
                selectors.update(selector_usage(prelude))
            group = at_rule and len(significant) > 1 and significant[1][1].lower() in ('media', 'supports', 'layer', 'container')
            contexts.append('group' if group else 'declarations')
            prelude = []
        elif kind == '}':
            if contexts:
                contexts.pop()
            prelude = []
        elif not contexts or contexts[-1] == 'group':
            prelude.append((kind, value))
            if kind == ';':
                prelude = []
    return dict(rules=rules, max_depth=depth, important=important,
                properties=dict(properties), at_rules=dict(at_rules), units=dict(units),
                selectors=dict(selectors), shorthand_shapes=dict(shorthands))


def package_path(base, reference):
    url = urlsplit(reference)
    if url.scheme or url.netloc or reference.startswith(('/', '\\')) or '\\' in reference:
        return None
    value = posixpath.normpath(posixpath.join(posixpath.dirname(base), unquote(url.path)))
    return None if value == '..' or value.startswith(('../', '/')) else value


def imported_paths(source):
    ts = list(tokens(source))
    for i in range(len(ts) - 2):
        if ts[i][0] != '@' or ts[i + 1][1].lower() != 'import':
            continue
        target = ts[i + 2]
        if target[0] == 'string':
            yield target[1][1:-1]
        elif target[1].lower() == 'url' and i + 4 < len(ts) and ts[i + 3][0] == '(':
            end = i + 4
            while end < len(ts) and ts[end][0] != ')':
                end += 1
            value = ''.join(v for _, v in ts[i + 4:end])
            yield value.strip('\"\'')


def read_member(archive, name):
    if archive.getinfo(name).file_size > 16 * 1024 * 1024:
        raise ValueError('EPUB member exceeds the 16 MiB survey limit')
    return archive.read(name)


def survey(book, output, probe=None):
    output.mkdir(parents=True, exist_ok=False)
    summaries, chapters, inputs = [], [], []
    with zipfile.ZipFile(book) as archive:
        names = set(archive.namelist())
        if len(names) > 100000:
            raise ValueError('EPUB directory exceeds the 100000-entry survey limit')
        def associated_css(name, active=(), budget=None):
            if budget is None:
                budget = [0, 0]
            if name in active:
                return [], 1
            if len(active) >= 32:
                return [], 1
            source = read_member(archive, name).decode('utf-8-sig', errors='replace')
            budget[0] += len(source.encode())
            budget[1] += 1
            if budget[0] > 16 * 1024 * 1024 or budget[1] > 4096:
                raise ValueError('Associated CSS exceeds the survey byte or import count limit')
            sources, skipped = [], 0
            for reference in imported_paths(source):
                path = package_path(name, reference)
                if path in names:
                    imported, failures = associated_css(path, active + (name,), budget)
                    sources.extend(imported)
                    skipped += failures
            sources.append(source)
            if sum(len(s) for s in sources) > 16 * 1024 * 1024:
                raise ValueError('Associated CSS exceeds the 16 MiB survey limit')
            return sources, skipped
        benchmark = None
        for name in sorted(names):
            if name.lower().endswith('.css'):
                data = read_member(archive, name)
                text = data.decode('utf-8-sig', errors='replace')
                dest = output / f'{len(inputs):04d}.css'
                dest.write_bytes(data)
                usage = css_usage(text)
                usage['bytes'] = len(data)
                if probe:
                    result = subprocess.run([str(probe), str(dest)], check=True, text=True, capture_output=True)
                    usage['prototype'] = json.loads(result.stdout)
                summaries.append(usage)
                inputs.append(name)
        for name in sorted(names):
            if not name.lower().endswith(('.xhtml', '.html', '.htm')):
                continue
            raw = read_member(archive, name)
            fallback = False
            try:
                root = ET.fromstring(raw)
            except ET.ParseError:
                parser = HtmlTree()
                parser.feed(raw.decode('utf-8-sig', errors='replace'))
                parser.close()
                root = parser.root
                fallback = True
            associated, inline, style_blocks, inline_blocks, import_failures = [], 0, [], [], 0
            for element in root.iter():
                tag = element.tag.rsplit('}', 1)[-1].lower()
                if tag == 'link' and 'stylesheet' in element.get('rel', '').lower().split():
                    path = package_path(name, element.get('href', ''))
                    if path in names:
                        sources, failures = associated_css(path)
                        associated.extend(sources)
                        import_failures += failures
                elif tag == 'style':
                    style_blocks.append(''.join(element.itertext()))
                if element.get('style') is not None:
                    inline += 1
                    inline_blocks.append('_ {' + element.get('style') + '}')
            css = '\n'.join(associated + style_blocks)
            usage = css_usage(css)
            usage.update(bytes=len(css.encode()), elements=sum(1 for _ in root.iter()), inline=inline,
                         import_cycles_or_depth_limits=import_failures, html_fallback=fallback)
            usage['inline_bytes'] = sum(len(node.get('style', '').encode()) for node in root.iter())
            pending, element_depth = [(root, 1)], 0
            while pending:
                current, level = pending.pop()
                element_depth = max(element_depth, level)
                pending.extend((child, level + 1) for child in current)
            usage['element_depth'] = element_depth
            usage['element_rule_product'] = usage['elements'] * usage['rules']
            chapters.append(usage)
            if probe:
                result = subprocess.run([str(probe), '--stdin'], input=css, check=True, text=True, capture_output=True)
                usage['associated_prototype'] = json.loads(result.stdout)
            if style_blocks or inline_blocks:
                dest = output / f'inline-{len(chapters):04d}.css'
                dest.write_text('\n'.join(style_blocks + inline_blocks))
                if probe:
                    result = subprocess.run([str(probe), str(dest)], check=True, text=True, capture_output=True)
                    usage['prototype'] = json.loads(result.stdout)
            if benchmark is None or len(css) > len(benchmark['css']):
                elements = []
                pending = [(root, -1)]
                while pending and len(elements) < 256:
                    current, parent = pending.pop()
                    index = len(elements)
                    elements.append([current.tag.rsplit('}', 1)[-1], current.get('id', ''),
                                     current.get('class', ''), current.get('style', ''), parent])
                    pending.extend((child, index) for child in reversed(list(current)))
                benchmark = dict(css=css, elements=elements)
    with book.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    report = dict(sha256=digest, sheets=summaries, chapters=chapters)
    (output / 'report.json').write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n')
    if benchmark:
        (output / 'bench.json').write_text(json.dumps(benchmark, ensure_ascii=False))
    return report


def cpp_string(value):
    data = value.encode('utf-8')
    if not data:
        return '""'
    return '\n'.join('"' + ''.join(f'\\x{c:02x}' for c in data[start:start + 128]) + '"'
                     for start in range(0, len(data), 128))


def benchmark_header(output, destination):
    """Export private source to an explicitly chosen private build header."""
    lines, cases = [], []
    for index, path in enumerate(sorted(output.glob('*/bench.json'))):
        data = json.loads(path.read_text())
        lines.append(f'constexpr char css_{index}[] = {cpp_string(data["css"])};')
        lines.append(f'constexpr BenchElement elements_{index}[] = {{')
        for tag, identifier, classes, inline_style, parent in data['elements']:
            lines.append('{' + ','.join(cpp_string(v) for v in (tag, identifier, classes, inline_style)) + f',{parent}' + '},')
        lines.append('};')
        cases.append(f'{{std::string_view(css_{index}, sizeof(css_{index}) - 1),elements_{index},sizeof(elements_{index}) / sizeof(elements_{index}[0])}}')
    lines.append('constexpr BenchInput bench_inputs[] = {' + ','.join(cases) + '};')
    destination.write_text('\n'.join(lines) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--probe', type=Path)
    parser.add_argument('--bench-header', type=Path)
    args = parser.parse_args()
    books = sorted(args.source.rglob('*.epub'))
    args.output.mkdir(parents=True, exist_ok=False)
    reports = [survey(book, args.output / f'{index:03d}', args.probe) for index, book in enumerate(books)]
    summary = dict(books=len(reports), sheets=sum(len(r['sheets']) for r in reports),
                   chapters=sum(len(r['chapters']) for r in reports))
    for field in ('properties', 'at_rules', 'units', 'selectors', 'shorthand_shapes'):
        counts = Counter()
        for report in reports:
            for sheet in report['sheets']:
                counts.update(sheet[field])
        summary[field] = dict(counts.most_common())
    summary['max_associated_bytes'] = max((c.get('bytes', 0) for r in reports for c in r['chapters']), default=0)
    summary['max_element_rule_product'] = max((c.get('element_rule_product', 0) for r in reports for c in r['chapters']), default=0)
    (args.output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    if args.bench_header:
        benchmark_header(args.output, args.bench_header)
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
