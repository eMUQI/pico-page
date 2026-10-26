"""Small synthetic StarDict fixtures; no third-party dictionary content."""
import gzip
import pathlib
import struct
import sys
import zlib


def dictzip(data, block_size=83):
    compressor = zlib.compressobj(wbits=-15)
    chunks = []
    for start in range(0, len(data), block_size):
        chunks.append(compressor.compress(data[start:start + block_size]) + compressor.flush(zlib.Z_FULL_FLUSH))
    tail = compressor.flush(zlib.Z_FINISH)
    ra = struct.pack('<HHH', 1, block_size, len(chunks)) + b''.join(struct.pack('<H', len(c)) for c in chunks)
    extra = b'RA' + struct.pack('<H', len(ra)) + ra
    return (b'\x1f\x8b\x08\x04' + b'\0' * 6 + struct.pack('<H', len(extra)) + extra +
            b''.join(chunks) + tail + struct.pack('<II', zlib.crc32(data), len(data)))


def key(word):
    raw = word.encode()
    return raw.lower(), raw


def make(root, name, records, types='m', compressed=True, synonyms=(), block_size=83):
    records = sorted(records, key=lambda pair: key(pair[0]))
    data = b''
    index = b''
    for word, definition in records:
        index += word.encode() + b'\0' + struct.pack('>II', len(data), len(definition))
        data += definition
    base = root / name
    base.with_suffix('.dict.dz' if compressed else '.dict').write_bytes(dictzip(data, block_size) if compressed else data)
    base.with_suffix('.idx').write_bytes(index)
    metadata = ("StarDict's dict ifo file\nversion=3.0.0\nbookname=Synthetic fixture\n" +
                f'wordcount={len(records)}\nidxfilesize={len(index)}\n')
    if types:
        metadata += f'sametypesequence={types}\n'
    if synonyms:
        metadata += f'synwordcount={len(synonyms)}\n'
        syn = b''.join(w.encode() + b'\0' + struct.pack('>I', n) for w, n in sorted(synonyms, key=lambda p: key(p[0])))
        base.with_suffix('.syn.dz').write_bytes(dictzip(syn))
    base.with_suffix('.ifo').write_text(metadata)


def main():
    root = pathlib.Path(sys.argv[1])
    root.mkdir(parents=True, exist_ok=True)
    records = [('APPLE', b'upper'), ('Apple', b'title'), ('apple', b'lower <literal>')]
    records += [('duplicate', f'meaning {i:03} '.encode() + b'x' * 100) for i in range(600)]
    records += [('omega', ('long ' + '中文' * 700).encode()), ('中文', '词条释义'.encode())]
    make(root, 'compressed', records, synonyms=[('alias', 0), ('alias', 2), ('alias', 2), ('别名', 604)])
    make(root, 'query-short', [('中', '单字释义'.encode())])
    make(root, 'ui-entries', [('sample', f'entry {i:02d}'.encode()) for i in range(33)])
    make(root, 'repetitive', [('sample', b'a' * 100000)], block_size=60000)
    make(root, 'han-short', [('中', b'short match')])
    make(root, 'han-long', [('中文', b'long match')])
    make(root, 'plain', [('apple', b'lower <literal>')], compressed=False)
    (root / 'long-name.ifo').write_text((root / 'plain.ifo').read_text().replace('Synthetic fixture', 'x' * 513))
    make(root, 'fields', [('sample', b'tphonetic\0mplain <literal>\0h<p>HTML</p>\0g<b>Pango</b>\0x<k>word</k>\0W\0\0\0\3\0\1\2')], types='')
    make(root, 'tm', [('sample', '音标'.encode() + b'\0definition')], types='tm')
    make(root, 'empty-last', [('sample', b'phonetic\0')], types='tm')
    make(root, 'binary-last', [('sample', b'text\0\0\1\2')], types='mW')
    make(root, 'known-only', [('sample', b'text')])
    rich = ''.join(f'<p><b>{i:03d}.</b> Definition with an example.</p>' for i in range(1200))
    make(root, 'ui-rich', [('sample', ('fəˈnetɪk\0' + rich).encode()),
                           ('abcdefgh' * 8, 'əˈlɒŋ\0<p>A long query remains readable.</p>'.encode()),
                           ('missing', '\0<p>Missing glyph: \U0010ffff</p>'.encode()),
                           ('發', '\0<p>A traditional headword.</p>'.encode())], types='th')
    title = root / 'ui-rich.ifo'
    title.write_text(title.read_text().replace('Synthetic fixture', 'Synthetic ' + 'x' * 502))
    make(root, 'unknown-text', [('sample', b'text\0\xff')], types='ml')
    make(root, 'unsupported-only', [('sample', b'\0\1\2')], types='W')
    make(root, 'invalid-field', [('sample', b'tmissing-terminator')], types='')
    make(root, 'invalid-utf8', [('sample', b'\xc0\x80')])
    make(root, 'oversized', [('sample', b'x' * (128 * 1024 + 1))], compressed=False)
    make(root, 'packed-large', [(f'word{i:05}', b'definition') for i in range(4096)], compressed=False)
    p = root / 'packed-large.idx'
    (root / 'packed-large.idx.gz').write_bytes(gzip.compress(p.read_bytes()))
    p.unlink()
    for name in ('packed', 'gzip-crc', 'gzip-truncated', 'gzip-multi'):
        make(root, name, [('apple', b'word'), ('banana', b'fruit')])
        raw = (root / (name + '.idx')).read_bytes()
        (root / (name + '.idx.gz')).write_bytes(gzip.compress(raw))
        (root / (name + '.idx')).unlink()
    p = root / 'packed.ifo'
    (root / 'packed.ifo.gz').write_bytes(gzip.compress(p.read_bytes()))
    p.unlink()
    for filename in ('packed.ifo.gz', 'packed.idx.gz'):
        p = root / filename
        raw = p.read_bytes()
        header = raw[:3] + bytes([raw[3] | 2]) + raw[4:10]
        p.write_bytes(header + struct.pack('<H', zlib.crc32(header) & 65535) + raw[10:])
    p = root / 'gzip-crc.idx.gz'
    raw = bytearray(p.read_bytes()); raw[-8] ^= 1; p.write_bytes(raw)
    p = root / 'gzip-truncated.idx.gz'; raw = p.read_bytes(); p.write_bytes(raw[:-9] + raw[-8:])
    p = root / 'gzip-multi.idx.gz'; p.write_bytes(p.read_bytes() + gzip.compress(b''))
    for name in ('offset64', 'unsorted', 'bad-range', 'bad-size', 'bad-ra', 'bad-block', 'gzip', 'truncated'):
        make(root, name, [('apple', b'apple'), ('banana', b'banana')])
    p = root / 'offset64.ifo'
    p.write_text(p.read_text() + 'idxoffsetbits=64\n')
    p = root / 'unsorted.idx'
    data = p.read_bytes(); p.write_bytes(data[14:] + data[:14])
    p = root / 'bad-range.idx'
    data = bytearray(p.read_bytes()); data[6:10] = b'\xff' * 4; p.write_bytes(data)
    p = root / 'bad-size.ifo'
    p.write_text(p.read_text().replace('idxfilesize=29', 'idxfilesize=30'))
    p = root / 'bad-ra.dict.dz'
    data = bytearray(p.read_bytes()); data[20:22] = b'\xff\xff'; p.write_bytes(data)
    p = root / 'bad-block.dict.dz'
    data = bytearray(p.read_bytes()); data[24] = 7; p.write_bytes(data)
    (root / 'gzip.dict.dz').write_bytes(gzip.compress(b'applebanana'))
    p = root / 'truncated.dict.dz'; p.write_bytes(p.read_bytes()[:-3])


if __name__ == '__main__':
    main()
