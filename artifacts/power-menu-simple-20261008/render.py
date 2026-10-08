"""Build the host-only capture helper and encode its raw grayscale canvas as PNG."""
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[2]
OUT = Path(__file__).resolve().parent
BUILD = ROOT / "build"
flags = (BUILD / "CMakeFiles/app_test.dir/flags.make").read_text()
includes = next(line.split(" = ", 1)[1] for line in flags.splitlines() if line.startswith("CXX_INCLUDES ="))
libs = shlex.split((BUILD / "CMakeFiles/app_test.dir/link.txt").read_text().split("-o app_test", 1)[1])
libs = [str(BUILD / p) if not p.startswith("/") else p for p in libs]

def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

with tempfile.TemporaryDirectory(prefix="pico-power-capture-") as temp:
    temp = Path(temp)
    subprocess.run(["c++", "-std=c++17", *shlex.split(includes), str(OUT / "render.cpp"),
                    "-o", str(temp / "render"), *libs], check=True)
    subprocess.run([str(temp / "render"), str(temp / "card"),
                    str(BUILD / "document-fixtures/valid.epub"), str(temp / "frames")], check=True)
    for path in (temp / "frames").glob("*.pgm"):
        magic, size, depth, pixels = path.read_bytes().split(b"\n", 3)
        width, height = map(int, size.split())
        assert magic == b"P5" and depth == b"255" and len(pixels) == width * height
        rows = b"".join(b"\0" + pixels[y * width:(y + 1) * width] for y in range(height))
        png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0))
        png += chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")
        (OUT / (path.stem + ".png")).write_bytes(png)
