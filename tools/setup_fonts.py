#!/usr/bin/env python3
"""Fetch checksum-pinned renderer and font resources into the ignored .local tree."""
import argparse
import hashlib
from pathlib import Path
import shutil
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
REVISION = "526ec5c47b9ebccc4754c85ac0c0cdf7c85a5e9b"
ASSETS = (
    ("freetype.tar.gz", f"https://codeload.github.com/freetype/freetype/tar.gz/{REVISION}",
     "d24f53b60034dc5c48e90d122d432a8f04a05b9e0a98c8d8e49b4a759207ce2e"),
    ("LXGWWenKai-Regular.ttf", "https://github.com/lxgw/LxgwWenKai/releases/download/v1.520/LXGWWenKai-Regular.ttf",
     "8d6ba638ac9553413354cfaab97637c1cd778444e259441ea1e5f8fb2c697fba"),
    ("NotoSansSC-Regular.otf", "https://raw.githubusercontent.com/notofonts/noto-cjk/523d033d6cb47f4a80c58a35753646f5c3608a78/Sans/SubsetOTF/SC/NotoSansSC-Regular.otf",
     "faa6c9df652116dde789d351359f3d7e5d2285a2b2a1f04a2d7244df706d5ea9"),
)


def fetch(path, url, digest):
    if path.exists() and hashlib.sha256(path.read_bytes()).hexdigest() == digest:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".download")
    try:
        with urllib.request.urlopen(url, timeout=60) as source, temporary.open("wb") as target:
            shutil.copyfileobj(source, target)
        if hashlib.sha256(temporary.read_bytes()).hexdigest() != digest:
            raise ValueError(f"SHA-256 mismatch: {url}")
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--card", type=Path, default=ROOT / ".local/card")
    args = parser.parse_args()
    fonts = args.card.resolve() / "PicoPage/fonts"
    for name, url, digest in ASSETS:
        destination = ROOT / ".local/downloads" / name if name.endswith(".gz") else fonts / name
        fetch(destination, url, digest)
        print(f"Verified {destination}")
    source = ROOT / ".local" / f"freetype-{REVISION}"
    if not source.exists():
        with tarfile.open(ROOT / ".local/downloads/freetype.tar.gz") as archive:
            for member in archive.getmembers():
                path = Path(member.name)
                if path.is_absolute() or ".." in path.parts or member.issym() or member.islnk():
                    raise ValueError("Unsafe source archive member")
            archive.extractall(ROOT / ".local")
    licenses = ROOT / "firmware/components/reader_graphics/fonts"
    for name in ("LXGW-OFL.txt", "Noto-OFL.txt"):
        shutil.copyfile(licenses / name, fonts / name)


if __name__ == "__main__":
    main()
